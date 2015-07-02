/**
 * Copyright (c) 2012  Dirk Hutter <hutter@compeng.uni-frankfurt.de>
 **/

// server class to send dcm and dlm from zmq

#include <vector>
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <chrono>
#include <sys/eventfd.h>
#include <boost/thread.hpp>

#include <log.hpp>
#include <flib.h>
#include <zmq.hpp>

class flib_ctrl_server {

  zmq::context_t& _zmq_context;
  std::string _path;
  flib::flib_device_cnet& _device;
  flib::flib_link_cnet& _link;
  zmq::socket_t _driver_req;
  zmq::socket_t _driver_res;
  boost::thread _driver_thread;
  int _stop_fd;
  std::vector<zmq_pollitem_t> _poll_items;

  enum driver_state_t {
    DriverStateStopped,
    DriverStateRunning,
    DriverStateStopping
  };
  driver_state_t _driver_state;

public:
  flib_ctrl_server(zmq::context_t& context,
                   std::string path,
                   flib::flib_device_cnet& device,
                   flib::flib_link_cnet& link)
      : _zmq_context(context), _path(path), _device(device), _link(link),
        _driver_req(context, ZMQ_PULL), _driver_res(context, ZMQ_PUSH),
        _stop_fd(-1), _driver_state(DriverStateStopped) {}

  ~flib_ctrl_server() { Stop(); }

  void Bind() {
    std::string req = "inproc://" + _path + "req";
    std::string res = "inproc://" + _path + "res";
    ;
    _driver_req.bind(req.c_str());
    _driver_res.bind(res.c_str());
    return;
  }

  bool Start() {
    // setup fd for stopping driver
    _stop_fd = ::eventfd(0, 0);
    if (_stop_fd < 0) {
      return false;
    }

    // setup poll list
    _poll_items.clear();
    zmq_pollitem_t zpi;
    zpi.socket = 0;
    zpi.fd = _stop_fd;
    zpi.events = ZMQ_POLLIN;
    zpi.revents = 0;
    _poll_items.push_back(zpi);

    zpi.socket = _driver_req;
    zpi.fd = 0;
    _poll_items.push_back(zpi);

    // start driver thread
    _driver_thread =
        boost::thread(boost::bind(&flib_ctrl_server::Driver, this));

    return true;
  }

  void Stop() {
    // if driver is already stopped do nothing
    if (_driver_state == DriverStateStopped)
      return;

    // send stop event to driver thread and join thread
    if (_driver_state != DriverStateStopped) {
      uint64_t one(1);
      int irc = ::write(_stop_fd, &one, sizeof(one));
      if (irc != sizeof(one)) {
        throw 1; // TODO
      }
    }

    if (_driver_thread.get_id() != boost::thread::id()) {
      if (!_driver_thread.timed_join(boost::posix_time::milliseconds(500))) {
        throw 1; // TODO
      }
    }

    // close wakeup eventfd
    if (_stop_fd > 0)
      ::close(_stop_fd);
    _stop_fd = -1;

    _poll_items.clear();

    return;
  }

  void Driver() {
    _driver_state = DriverStateRunning;
    // event loop
    while (_driver_state == DriverStateRunning) {
      // poll for events
      int rc = zmq::poll(_poll_items.data(), _poll_items.size(), -1);
      if (rc == -1 && errno == EINTR)
        continue;

      for (size_t i = 0; i < _poll_items.size(); i++) {
        if (_poll_items[i].revents == 0)
          continue;
        if (_poll_items[i].fd == _stop_fd) {
          // stop driver
          uint64_t ret;
          ssize_t rc = ::read(_stop_fd, &ret, sizeof(ret));
          if (rc != -1 && ret == 1) {
            _driver_state = DriverStateStopped;
          }
        } else if (_poll_items[i].socket == _driver_req) {
          // process message
          ProcEvent();
        }
      }
    }
    return;
  }

  void ProcEvent() {
    flib::flib_link_cnet::ctrl_msg_t cnet_s_msg;

    // get messsage
    size_t msg_size =
        _driver_req.recv(cnet_s_msg.data, sizeof(cnet_s_msg.data));
    if (msg_size > sizeof(cnet_s_msg.data)) {
      cnet_s_msg.words = sizeof(cnet_s_msg.data) / sizeof(cnet_s_msg.data[0]);
      L_(error) << "Message truncated";
    } else {
      cnet_s_msg.words = msg_size / sizeof(cnet_s_msg.data[0]);
    }

    // DEBUG
    for (size_t i = 0; i < cnet_s_msg.words; i++) {
      L_(trace) << "msg to send " << std::hex << std::setfill('0') << "0x"
                << std::setw(4) << cnet_s_msg.data[i];
    }

    // TODO: Hack! single word request is DLM request
    if (msg_size / sizeof(uint16_t) == 1) {
      SendDlm(cnet_s_msg);
    }
    // single double word is flib read
    else if (msg_size == sizeof(uint32_t)) {
      FlibRead(cnet_s_msg);
    }
    // double doule word is flib write
    else if (msg_size == 2 * sizeof(uint32_t)) {
      FlibWrite(cnet_s_msg);
    } else {
      SendCtrl(cnet_s_msg);
    }

    return;
  }

  void SendCtrl(flib::flib_link_cnet::ctrl_msg_t& cnet_s_msg) {
    flib::flib_link_cnet::ctrl_msg_t cnet_r_msg;

    L_(info) << "sending control message";

    // receive to flush hw buffers
    if (_link.recv_dcm(&cnet_r_msg) != -1) {
      L_(warning) << "sprious message dropped";
    }

    // send command
    if (_link.send_dcm(&cnet_s_msg) < 0) {
      L_(error) << "sending message failed";
    }

    // receive response
    int ret = -1;
    int timeout = 900; // ControlServer has 1000 µs timeout
    auto timeout_tp = std::chrono::high_resolution_clock::now() +
                      std::chrono::microseconds(timeout);
    // poll till msg is available, error occures or timeout is reached
    while (ret == -1 &&
           std::chrono::high_resolution_clock::now() < timeout_tp) {
      ret = _link.recv_dcm(&cnet_r_msg);
    }

    if (ret == -2) {
      L_(error) << "received message with illegal size";
    } else if (ret == -1) {
      L_(error) << "timeout receiving message";
    } else {
      // DEBUG
      for (size_t i = 0; i < cnet_r_msg.words; i++) {
        L_(trace) << "msg received " << std::hex << std::setfill('0') << "0x"
                  << std::setw(4) << cnet_r_msg.data[i];
      }
    }

    // response to driver no matter what happend
    _driver_res.send(cnet_r_msg.data,
                     cnet_r_msg.words * sizeof(cnet_r_msg.data[0]));
    return;
  }

  void SendDlm(flib::flib_link_cnet::ctrl_msg_t& cnet_s_msg) {
    L_(info) << "Sending DLM " << std::hex << "0x" << cnet_s_msg.data[0];

    // set dlm config for single link
    _link.prepare_dlm((cnet_s_msg.data[0] & 0xF), true);

    // send dlm
    _link.send_dlm();

    return;
  }

  void FlibRead(flib::flib_link_cnet::ctrl_msg_t& cnet_s_msg) {
    L_(debug) << "Reading FLIB link register: " << std::hex << "addr "
              << cnet_s_msg.data[0];

    // TODO read acctual flib register
    uint32_t val = 0x1 + cnet_s_msg.data[0];

    _driver_res.send(&val, sizeof(uint32_t));

    return;
  }

  void FlibWrite(flib::flib_link_cnet::ctrl_msg_t& cnet_s_msg) {
    uint32_t addr = cnet_s_msg.data[1] << 16 | cnet_s_msg.data[0];
    uint32_t data = cnet_s_msg.data[3] << 16 | cnet_s_msg.data[2];

    L_(debug) << "Writing FLIB link register: " << std::hex << "addr " << addr
              << " data " << data;

    return;
  }
};
