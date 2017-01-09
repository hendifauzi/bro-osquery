#include <osquery/sdk.h>
#include <osquery/system.h>

#include "logger/logger.h"
#include "BrokerManager.h"
#include <utils.h>

#include <broker/broker.hh>
#include <broker/endpoint.hh>
#include <broker/message_queue.hh>
#include <broker/report.hh>

#include <iostream>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

using namespace osquery;

REGISTER_EXTERNAL(BroLoggerPlugin, "logger", "bro");

void signalHandler( int signum ) {
  LOG(INFO) << "Interrupt signal (" << signum << ") received";

  // TODO Announce this node goes offline  

  exit(signum);
}

int main(int argc, char* argv[]) {

  signal(SIGINT, signalHandler);
  
  // Setup OSquery Extension
  Initializer runner(argc, argv, ToolType::EXTENSION);
  auto status_ext = startExtension("bro-osquery", "0.0.1");
  if (!status_ext.ok()) {
    LOG(ERROR) << status_ext.getMessage();
    runner.requestShutdown(status_ext.getCode());
  }

  // Setup Broker Endpoint
  broker::init();
  BrokerManager* bm = BrokerManager::getInstance();
  bm->createEndpoint("Bro-osquery Extension");
  // Listen on default topics (global, groups and node)
  bm->createMessageQueue("/osquery/all");
  auto groups = bm->getGroups();
  for (std::string g: groups) {
    bm->createMessageQueue("/osquery/group/" + g);
  }
  std::string uid = bm->getNodeID();
  bm->createMessageQueue("/osquery/uid/" + uid);
  // Connect to Bro
  auto status_broker = bm->peerEndpoint("172.17.0.2", 9999);
  if (!status_broker.ok()) {
    LOG(ERROR) << status_broker.getMessage();
    runner.requestShutdown(status_broker.getCode());
  }

  // Annouce this endpoint to be a bro-osquery extension
  broker::message announceMsg = broker::message{"new_osquery_host", uid};
  bm->getEndpoint()->send("/osquery/announces", announceMsg);

  // Wait for schedule requests
  fd_set fds;
  std::vector<std::string> topics;
  int sock;
  broker::message_queue* queue = nullptr;
  int sMax;
  while (true) {
    // Retrieve info about each message queue
    FD_ZERO(&fds);
    bm->getTopics(topics); // List of subscribed topics
    sMax = 0;
    for (auto topic: topics) {
      sock = bm->getMessageQueue(topic)->fd();
      if (sock > sMax) {sMax = sock;}
      FD_SET(sock, &fds); // each topic -> message_queue -> fd
    }
    // Wait for incoming message
    if ( select(sMax + 1, &fds, NULL, NULL, NULL) < 0) {
      LOG(ERROR) << "Select returned an error code";
      continue;
    }
    
    // Check for the socket where a message arrived on
    for (auto topic: topics) {
      queue = bm->getMessageQueue(topic);
      sock = queue->fd();
      if ( FD_ISSET(sock, &fds) ) {
    
        // Process each message on this socket
        for ( auto& msg: queue->want_pop() ) {
          // Check Event Type
          std::string eventName = broker::to_string(msg[0]);
          LOG(INFO) << "Received event '" << eventName << "' on topic '" << topic << "'";
          
          if ( eventName == "add_osquery_query" ) {
          // New SQL Query Request
            QueryRequest qr;
            qr.query = broker::to_string(msg[2]);
            qr.response_event = broker::to_string(msg[1]);
            // The topic where the request was received
            qr.response_topic = topic; // TODO: or use custom as optionally specified in msg
            bm->addBrokerQueryEntry(qr);

          } else if ( eventName == "remove_osquery_query" ) {
          // SQL Query Cancel
            //TODO: find an UNIQUE identifier (currently the exact sql string)
            std::string query = broker::to_string(msg[1]);
            bm->removeBrokerQueryEntry(query);

          } else {
          // Unkown Message
            LOG(ERROR) << "Unknown Event Name: '" << eventName << "'";
            LOG(ERROR) << "\t" << broker::to_string(msg);
            continue;
          }

          // Apply to new config/schedule
          std::map<std::string, std::string> config;
          config["data"] = bm->getQueryConfigString();
          LOG(INFO) << "Applying new config: " << config["data"];
          Config::getInstance().update(config);
        }
      }
    }
  }

  LOG(ERROR) << "What happened here?";
  // Finally wait for a signal / interrupt to shutdown.
  runner.waitForShutdown();
  return 0;
}