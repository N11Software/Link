/* 
 *  ______________________________________
 * |  _   _  _  _                         |
 * | | \ | |/ |/ |     Date: 06/23/2022   |
 * | |  \| |- |- |     Author: Levi Hicks |
 * | |     || || |                        |
 * | | |\  || || |                        |
 * | |_| \_||_||_|     File: Link.cpp     |
 * |                                      |
 * |                                      |
 * | Please do not remove this header.    |
 * |______________________________________|
 */

#include "../Includes/Link/Link.hpp"
#include <iostream>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <filesystem>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <stdexcept>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <vector>
#include <bits/stdc++.h>
#include "../Includes/Link/HTTPTools.hpp"

/*
 * Holds data for the request.
 */
class ThreadInfo {
  public:
    std::function<void(Request*, Response*)> func;
    Request* request;
    std::map<int, std::function<void(Request*, Response*)>> errorHandlers;
};

/*
 * Creates a new thread.
 * We have to use this because pthread_create will not hold the function and the Request.
 * 
 * @param info The information to pass to the thread.
 */
void* threadWrapper(void* arg) {
  ThreadInfo* info = (ThreadInfo*)arg;
  Response response(info->request, &info->errorHandlers);
  info->func(info->request, &response);
  if (!response.IsCancelled()) {
    int packet = 0;
    while (packet < response.GetHTTP().size()) {
      int packetLength = response.GetHTTP().size()<packet+1024? response.GetHTTP().size()-packet: 1024;
      std::string PacketData = response.GetHTTP().substr(packet, packetLength);
      int sent = send(info->request->GetSocket(), PacketData.c_str(), PacketData.size(), 0);
      if (sent < 0) return NULL;
      packet+=packetLength;
    }
    close(info->request->GetSocket());
  } else {
    close(info->request->GetSocket());
  }
  return NULL;
}

/*
 * Creates a Link server
 *
 * @param port The port that the server will run on.
 */
Link::Link(int port) { this->port = port; }

/*
 * Set handler for a get request.
 *
 * @param path The specific path for the handler.
 * @param callback The function to call when a get request is made.
 */
void Link::Get(std::string path, std::function<void(Request*, Response*)> callback) {
  this->handlers[path+"GET"] = callback;
}

/*
 * Sets default request handler.
 *
 * @param callback The function to call when a request is made.
 */
void Link::Default(std::function<void(Request*, Response*)> callback) {
  this->defaultHandler = callback;
}

/*
 * Set handler for a post request.
 *
 * @param path The specific path for the handler.
 * @param callback The function to call when a post request is made.
 */
void Link::Post(std::string path, std::function<void(Request*, Response*)> callback) {
  this->handlers[path+"POST"] = callback;
}

/*
 * Set handler for a put request.
 *
 * @param path The specific path for the handler.
 * @param callback The function to call when a put request is made.
 */
void Link::Put(std::string path, std::function<void(Request*, Response*)> callback) {
  this->handlers[path+"PUT"] = callback;
}

/*
 * Set handler for a delete request.
 *
 * @param path The specific path for the handler.
 * @param callback The function to call when a delete request is made.
 */
void Link::Delete(std::string path, std::function<void(Request*, Response*)> callback) {
  this->handlers[path+"DELETE"] = callback;
}

/*
 * Set handler for an error.
 *
 * @param code The error code.
 * @param callback The function to call when an error is made.
 */
void Link::Error(int code, std::function<void(Request*, Response*)> callback) {
  this->errorHandlers[code] = callback;
}

/*
 * Binds and listens on specified port
 *
 * @return the error code.
 */
int Link::Start() {
  this->sock = socket(AF_INET, SOCK_STREAM, 0);
  if (this->sock == 0) return 1;
  int opt = 1;
  if (setsockopt(this->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) return 2;
  int time = 7200, interval = 60, retry = 3;
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(this->port);
  if (bind(this->sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) return 3;
  if (listen(this->sock, 50) < 0) return 4;
  while (true) {
    char buffer[1024] = {0};
    struct sockaddr_in addr;
    socklen_t addrSize = sizeof(addr);
    int sock = accept(this->sock, (struct sockaddr*)&addr, &addrSize);
    recv(sock, buffer, 1024, 0);
    std::string bufferStr = buffer, line, path, method;
    std::map<std::string, std::string> queriesMap;
    std::istringstream stream(decodeHTTP(bufferStr));
    getline(stream, line);
    if (line.substr(0, 3) == "GET" || line.substr(0, 4) == "POST" || line.substr(0, 3) == "PUT" || line.substr(0, 6) == "DELETE") {
      method = line.substr(0, 3) == "GET" ? "GET" : line.substr(0, 4) == "POST" ? "POST" : line.substr(0, 3) == "PUT"? "PUT" : "DELETE";
      path = line.substr(method.length()+1);
      path = path.substr(0, path.find_last_of("HTTP/1.1")-8);
    } else {
      close(sock);
      continue;
    }
    if (path.find("?") != std::string::npos) {
      std::string queries = path.substr(path.find_last_of("?")+1);
      path = path.substr(0, path.find_last_of("?"));
      std::istringstream stream(queries);
      std::string query;
      while(getline(stream, query, '&')) {
        std::string key = query.substr(0, query.find_first_of("="));
        std::string value = query.substr(query.find_first_of("=")+1);
        queriesMap[key] = sanitize(value);
      }
      std::string newPath = "";
      for (int i=0; i<path.length(); i++) if (path.substr(i, 2) != "//") newPath += path[i];
      path = newPath[newPath.length()-1]=='/' ? newPath.substr(0, newPath.length()-1) : newPath;
    }
    Request request(sock, &addr, path, method, buffer, queriesMap);
    if (this->handlers.contains(path+method)) {
      ThreadInfo* info = new ThreadInfo();
      info->func = this->handlers.find(path+method)->second;
      info->request = &request;
      info->errorHandlers = this->errorHandlers;
      pthread_t thread;
      pthread_create(&thread, NULL, threadWrapper, info);
      pthread_join(thread, NULL);
    } else if (this->defaultHandler != nullptr) {
      ThreadInfo* info = new ThreadInfo();
      info->func = this->defaultHandler;
      info->request = &request;
      info->errorHandlers = this->errorHandlers;
      pthread_t thread;
      pthread_create(&thread, NULL, threadWrapper, info);
      pthread_join(thread, NULL);
    } else {
      close(sock);
      shutdown(this->sock, SHUT_RDWR); 
      return 5;
    }
  }
  return 0;
}

/*
 * Shuts down the server
 *
 * @return the error code.
 */
int Link::Stop() {
  return shutdown(this->sock, SHUT_RDWR);
  return 0;
}

/*
 * @return the port number
 */
int Link::GetPort() { return this->port; }