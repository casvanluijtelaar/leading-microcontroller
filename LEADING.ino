#include <WiFi.h>
#include <FirebaseESP32.h>
#include "ArduinoJson.h"
#include <FastLED.h>

#include<list>
#include<array>
#include<string>

using namespace std;

// the id of this location aka the major value of the BLE Beacon
#define HUB_ID 1

// information for connecting to wifi and database
#define WIFI_SSID "SSID"
#define WIFI_PASSWORD "PASSWORD"
#define FIREBASE_HOST "HOST"
#define FIREBASE_AUTH "AUTH"

FirebaseData db;

// every led channel has exactly 19 leds spread over 33.3cm
#define NUM_LEDS_PER_STRIP 19

// channels are connected to 4 adjacent pins
#define channel0_pin 14
CRGB channel0[NUM_LEDS_PER_STRIP];
#define channel1_pin 27
CRGB channel1[NUM_LEDS_PER_STRIP];
#define channel2_pin 26
CRGB channel2[NUM_LEDS_PER_STRIP];
#define channel3_pin 25
CRGB channel3[NUM_LEDS_PER_STRIP];

// Hub details data model
struct Hub {
  public:
    int id; // unique identifier for this hub
    array<int, 2> direction; // stores incomming and outgoing channels
};

// user data model
struct User {
  public:
    String id; // unique identifier for this user
    String path; // the location of the user in the database
    array<int, 3> color; // unique color assigned to this user
    Hub hub; // hub object storing led path data
    int position; // the current led position of this user in this hub

  // two users are the same when they have the 
  // same database location
  bool operator ==(const User& u) {
    return u.path == path;
  }
};

// a list holding all active users on this hub
list<User> activeUsers;


// make sure that two users aren't occupying the same position
// so that the LEDs don't overlap
int findFreeLocation() {
  int freePosition = 0;
  for(User& user : activeUsers) {
   if(user.position == freePosition) {
     freePosition++;
   }
  }
  return freePosition;
}

// deserialises a user from the provided streamdata and adds it
// to the ative users list
void addUser(StreamData data) {
    // arduinoJson used for deserialisation
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, data.jsonString());

    // elements stored as basic types
    String id = doc["id"];
    String path = data.dataPath();
    JsonArray colorArray = doc["color"].as<JsonArray>();
    array<int, 3> color = {colorArray[0], colorArray[1], colorArray[2]};
    
    // one user can have multiple hubs
    JsonArray hubs = doc["hubs"].as<JsonArray>();

    // loop over all hubs
    for(JsonVariant hub : hubs) {

      // we only want to add the user to this hub if the user
      // object actualy contains a hub matching this HUB_ID
      int hubId = hub["id"];
      if (hubId != HUB_ID) continue;

      // create an User object from the deserialised data
      Hub hubObject = { hubId, {hub["direction"][0], hub["direction"][1]} };
      int position = findFreeLocation();
      User user = { id, path, color, hubObject, position };
      // add the user to the activeUsers list
      activeUsers.push_back(user);
      break;
    }
}

// attempts to remove a user from activeUsers
void removeUser(StreamData data) {
  User u = {"", data.dataPath()};
  activeUsers.remove(u);
}

// every time a user gets added or removed from the /users
// database location, this callback fires
void streamCallback(StreamData data) {

  // if data type is json a user is added
  if(data.dataType() == "json") {
   addUser(data);
   Serial.println("user added");

  //if the data type is null a user is removed
  } else if(data.dataType() == "null") {
    removeUser(data);
    Serial.println("removed user");
  }
}

// if database stream gets interupted, 
// currently not doing anything, though required.
void streamTimeoutCallback(bool timeout) {
  if (timeout) Serial.println("Stream timeout, resume streaming...");
}

// attempt to connect to the provided ssid and password
void connectWifi(char* ssid, char* password) {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");

  // block code execution untill wifi is connected
  while (WiFi.status() != WL_CONNECTED) delay(300);
  Serial.println("Connected");
}

// attempt to connect to firebase and start listening for the user branch
void setupFirebase(String host, String id) {
  Firebase.begin(host, id);
  Firebase.reconnectWiFi(true);

  Firebase.beginStream(db, "/users");
  Firebase.setStreamCallback(db, streamCallback, streamTimeoutCallback, 8192);
}

// updates a specific led in a specific channel with a provided color
void updateLed(int channel, int led, array<int, 3> color) {
  if(channel == 0) channel0[led] = CRGB(color[0], color[1], color[2]);
  if(channel == 1) channel1[led] = CRGB(color[0], color[1], color[2]);
  if(channel == 2) channel2[led] = CRGB(color[0], color[1], color[2]);
  if(channel == 3) channel3[led] = CRGB(color[0], color[1], color[2]);
}

// it could happen where a new users led gets placed exactly on an existing user
// which has a chance of 1/76 for 4 channels. this method checks if a position 
// is active within a certain range
bool isPositionOccupied(int position, int range) {
  
  int start = position - range;
  int end = position + range;

  for(User& user : activeUsers) {
    // if the range is between 0 - 37 check if user position falls
    // within these bounds, if so return true
    if(user.position >= start && user.position <= end) return true;
    // if the start of the rang is less than 0, it should wrap back around
    // so check if the user position is greater than the negative end 
    // position subtracted from 37
    if(start < 0) 
      if (user.position >= 37 + start)
        return true;
    // if the end is larger than 37 it has wrap around so we have to check
    // if the user position is smaller than the remainder of end-37
    if(end > 37)
      if(user.position <= end - 37)
        return true;
  } 
  // if no user in range is found a new user can be placed here so return false
  return false;
}

// updates all the currently active leds to new positions
void updateLEDs() {
  // no active users, dont execute
  if(activeUsers.size() == 0) return;
  // loop over all users
  for(User& user : activeUsers) { 
    // wrap users position if it reaches the end (19+19=38 aka 0-37)
    user.position = user.position == 37 ? 0 : user.position +1;
    // a hub exists of two directions, incomming and outgoing, this is just
    // the position split in the middle
    int direction = user.position < 19 ? 0 : 1;
    // the direction is an array of two channels e.g. [0, 2] this means 
    // channel 0 is incomming and channel 2 is outgoing
    int channel = user.hub.direction[direction];
    // the incoming channel runs from 18-0 while the outgoing channel runs 
    // 0-18. so if direction is incoming then position is between 0-18 and
    // has to be turned around, if direction is outgoing, the position is
    // between 19-37 and has to be mapped to 0-18
    int led = direction == 0 ? 18 - user.position : user.position - 19;
    // update the led on the right channel with the right color
    updateLed(channel, led, user.color);
  }
}

void setup() {
  Serial.begin(115200);
  // create wifi connection
  connectWifi(WIFI_SSID, WIFI_PASSWORD);
  // create firebase stream
  setupFirebase(FIREBASE_HOST, FIREBASE_AUTH);
  // setup the 4 led channels 
  FastLED.addLeds<WS2812, channel0_pin, GRB>(channel0, NUM_LEDS_PER_STRIP); 
  FastLED.addLeds<WS2812, channel1_pin, GRB>(channel1, NUM_LEDS_PER_STRIP); 
  FastLED.addLeds<WS2812, channel2_pin, GRB>(channel2, NUM_LEDS_PER_STRIP); 
  FastLED.addLeds<WS2812, channel3_pin, GRB>(channel3, NUM_LEDS_PER_STRIP); 
}

void loop() {
    // remove all the previous LEDs
    FastLED.clear();
    // update led positions
    updateLEDs();
    // show the updated leds
    FastLED.show();
    // wait 50ms
    delay(50);

}
