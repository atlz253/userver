#include "sender.hpp"

#include <sys/socket.h>

#include <cassert>
#include <cerrno>

#include <logging/log.hpp>
#include <utils/strerror.hpp>

namespace engine {

Sender::Sender(const ev::ThreadControl& thread_control,
               TaskProcessor& task_processor, int fd,
               OnCompleteFunc&& on_complete)
    : ev::ThreadControl(thread_control),
      current_data_pos_(0),
      stopped_{false},
      on_complete_(std::move(on_complete)),
      socket_listener_(std::make_shared<SocketListener>(
          thread_control, task_processor, fd,
          SocketListener::ListenMode::kWrite,
          [this](int fd) { return SendData(fd); }, [this]() { DoStop(); })) {}

Sender::~Sender() { Stop(); }

void Sender::Start() { socket_listener_->Start(); }

void Sender::Stop() {
  socket_listener_->Stop();
  DoStop();
}

void Sender::SendData(std::string data,
                      std::function<void(size_t)>&& finish_cb) {
  assert(finish_cb);
  bool inserted = false;
  {
    std::lock_guard<Mutex> lock(data_queue_mutex_);
    if (!stopped_) {
      data_queue_.emplace_back(std::move(data), std::move(finish_cb));
      inserted = true;
    }
  }
  if (inserted) {
    socket_listener_->Notify();
  } else {
    try {
      finish_cb(0);
    } catch (const std::exception& ex) {
      LOG_ERROR() << "exception in finish_cb: " << ex.what();
    }
  }
}

size_t Sender::DataQueueSize() const {
  std::lock_guard<Mutex> lock(data_queue_mutex_);
  return data_queue_.size();
}

bool Sender::HasWaitingData() const {
  std::lock_guard<Mutex> lock(data_queue_mutex_);
  return !data_queue_.empty();
}

void Sender::DoStop() {
  if (stopped_.exchange(true)) return;

  for (;;) {
    std::function<void(size_t)> finish_cb;
    size_t pos;
    {
      std::lock_guard<Mutex> lock(data_queue_mutex_);
      if (data_queue_.empty()) break;
      finish_cb = std::move(data_queue_.front().finish_cb);
      pos = current_data_pos_;
      data_queue_.pop_front();
      current_data_pos_ = 0;
    }
    try {
      finish_cb(pos);
    } catch (const std::exception& ex) {
      LOG_ERROR() << "exception in finish_cb: " << ex.what();
    }
  }

  assert(!HasWaitingData());
  if (on_complete_) on_complete_();
}

inline const std::string& Sender::CurrentData(std::lock_guard<Mutex>&) {
  assert(!data_queue_.empty());
  return data_queue_.front().data;
}

Sender::Result Sender::SendCurrentData(std::lock_guard<Mutex>& lock, int fd) {
  if (data_queue_.empty()) return Result::kOk;
  ssize_t res =
      send(fd, CurrentData(lock).data() + current_data_pos_,
           CurrentData(lock).size() - current_data_pos_, MSG_NOSIGNAL);
  if (res < 0) {
    auto send_errno = errno;
    if (send_errno == EAGAIN || send_errno == EWOULDBLOCK)
      return Result::kAgain;
    LOG_WARNING() << "error in send: " << utils::strerror(send_errno);
    return Result::kError;
  }
  current_data_pos_ += res;
  assert(current_data_pos_ <= CurrentData(lock).size());
  return (current_data_pos_ < CurrentData(lock).size()) ? Result::kAgain
                                                        : Result::kOk;
}

Sender::Result Sender::SendData(int fd) {
  Result res;
  for (;;) {
    bool call_finish_cb = false;
    std::function<void(size_t)> finish_cb;
    size_t pos;

    res = Result::kOk;
    {
      std::lock_guard<Mutex> lock(data_queue_mutex_);
      if (data_queue_.empty()) break;
      res = SendCurrentData(lock, fd);
      if (res != Result::kOk) break;

      if (current_data_pos_ == CurrentData(lock).size()) {
        finish_cb = std::move(data_queue_.front().finish_cb);
        pos = current_data_pos_;
        call_finish_cb = true;
        data_queue_.pop_front();
        current_data_pos_ = 0;
      }
    }
    if (call_finish_cb) {
      try {
        finish_cb(pos);
      } catch (const std::exception& ex) {
        LOG_ERROR() << "exception in finish_cb: " << ex.what();
      }
    }
  }
  if (res == Result::kError || res == Result::kAgain) return res;
  if (on_complete_) on_complete_();
  return res;
}

}  // namespace engine
