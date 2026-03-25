#include "Checker/FiTx/Core/Logs.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Provide pipe2 only on macOS (not in older SDKs); Linux/glibc has it in
// unistd.h
#if defined(__APPLE__)
static int pipe2(int fd[2], int flags) {
  if (pipe(fd) < 0)
    return -1;
  if (flags & O_NONBLOCK) {
    fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | O_NONBLOCK);
    fcntl(fd[1], F_SETFL, fcntl(fd[1], F_GETFL) | O_NONBLOCK);
  }
  return 0;
}
#endif

namespace fitx {
struct EndPoints EndPoint::createEndPointPair() {
  int fd[2];
  if (pipe2(fd, O_NONBLOCK) < 0) {
    return EndPoints();
  }

  return EndPoints{ReadEndPoint(fd[0]), WriteEndPoint(fd[1])};
}

ReadEndPoint::ReadEndPoint() : EndPoint() {}
ReadEndPoint::ReadEndPoint(int fd) : EndPoint(fd) {}
std::string ReadEndPoint::readLog() {
  std::string log;
  char buffer[kBufferSize];
  ssize_t size = 0;

  while ((size = read(Fd(), buffer, kBufferSize)) > 0) {
    log.append(buffer, size);
  }

  return log;
}

WriteEndPoint::WriteEndPoint() : EndPoint() {}
WriteEndPoint::WriteEndPoint(int fd) : EndPoint(fd) {}
void WriteEndPoint::write_log(const std::string &log) {
  if (write(Fd(), log.data(), log.size()) < 0)
    llvm::errs() << "cannot write to log\n";
  return;
}

LoggingClient::LoggingClient() {
  end_points_ = EndPoint::createEndPointPair();
  buffer_.reserve(ReadEndPoint::kBufferSize * 10);
}

void LoggingClient::log(const std::string &log) { buffer_.append(log); }

void LoggingClient::flush() {
  WriteEndPoint &write_point = end_points_.write;
  if (write_point.valid())
    write_point.write_log(buffer_);
  else
    llvm::errs() << buffer_;
  buffer_.clear();
}

void LoggingClient::printLog() {
  if (end_points_.read.valid())
    llvm::errs() << end_points_.read.readLog();
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                              fitx::LoggingClient &client) {
  if (client.end_points_.read.valid())
    ostream << client.end_points_.read.readLog();

  return ostream;
}

LoggingClient &LoggingClient::operator<<(const std::string &log) {
  this->log(log);
  return *this;
}

void LoggingServer::addClient(LoggingClient *client) {
  clients_.push_back(client);
}

void LoggingServer::printClientLogs() {
  for (auto *client : clients_) {
    client->printLog();
  }
}

} // namespace fitx
