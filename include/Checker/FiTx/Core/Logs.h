#pragma once
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fitx {

struct EndPoints;
class EndPoint {
public:
  static const int kInvalidEndpoint = -1;

  static struct EndPoints createEndPointPair();
  EndPoint() : fd_(kInvalidEndpoint) {}

  bool valid() { return fd_ != kInvalidEndpoint; }

protected:
  EndPoint(int fd) : fd_(fd) {}
  int Fd() { return fd_; };

private:
  int fd_;
};

class ReadEndPoint : public EndPoint {
public:
  static const size_t kBufferSize = 10000;

  ReadEndPoint();
  ReadEndPoint(int fd);
  std::string readLog();
};

class WriteEndPoint : public EndPoint {
public:
  WriteEndPoint();
  WriteEndPoint(int fd);
  void write_log(const std::string &log);
};

struct EndPoints {
  ReadEndPoint read;
  WriteEndPoint write;
};

class LoggingClient {
public:
  LoggingClient();

  void log(const std::string &log);
  void flush();

  void printLog();

  LoggingClient &operator<<(const std::string &log);
  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                                       fitx::LoggingClient &client);

  std::string &getBuffer() { return buffer_; }

private:
  std::string buffer_;
  struct EndPoints end_points_;
};

class LoggingServer {
public:
  LoggingServer() = default;

  void addClient(LoggingClient *client);
  void printClientLogs();

private:
  std::vector<LoggingClient *> clients_;
};
} // namespace fitx
