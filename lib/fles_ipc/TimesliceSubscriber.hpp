// Copyright 2014 Jan de Cuveland <cmail@cuveland.de>
/// \file
/// \brief Defines the fles::TimesliceSubscriber class.
#pragma once

#include "Source.hpp"
#include "StorableTimeslice.hpp"
#include "Timeslice.hpp"
#include <boost/archive/binary_iarchive.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <string>
#include <zmq.hpp>

namespace fles {
/**
 * \brief The TimesliceSubscriber class receives serialized timeslice data sets
 * from a zeromq socket.
 */
template <class Base, class Derived> class Subscriber : public Source<Base> {
public:
  /// Construct timeslice subscriber receiving from given ZMQ address.
  Subscriber(const std::string& address, uint32_t hwm) {
    subscriber_.set(zmq::sockopt::rcvhwm, int(hwm));
    subscriber_.connect(address.c_str());
    subscriber_.set(zmq::sockopt::subscribe, "");
  }

  /// Delete copy constructor (non-copyable).
  Subscriber(const Subscriber&) = delete;
  /// Delete assignment operator (non-copyable).
  void operator=(const Subscriber&) = delete;

  ~Subscriber() override = default;

  /**
   * \brief Retrieve the next item.
   *
   * This function blocks if the next item is not yet available.
   *
   * \return pointer to the item, or nullptr if end-of-file
   */
  std::unique_ptr<Derived> get() { return std::unique_ptr<Derived>(do_get()); };

  [[nodiscard]] bool eos() const override { return eos_flag; }

private:
  Derived* do_get() override {
    if (eos_flag) {
      return nullptr;
    }

    zmq::message_t message;
    [[maybe_unused]] auto result = subscriber_.recv(message);

    boost::iostreams::basic_array_source<char> device(
        static_cast<char*>(message.data()), message.size());
    boost::iostreams::stream<boost::iostreams::basic_array_source<char>> s(
        device);
    boost::archive::binary_iarchive ia(s);

    Derived* sts = nullptr;
    try {
      sts = new Derived(); // NOLINT
      ia >> *sts;
    } catch (boost::archive::archive_exception& e) {
      delete sts; // NOLINT
      eos_flag = true;
      return nullptr;
    }
    return sts;
  }

  zmq::context_t context_{1};
  zmq::socket_t subscriber_{context_, ZMQ_SUB};

  bool eos_flag = false;
};

using TimesliceSubscriber = Subscriber<Timeslice, StorableTimeslice>;

} // namespace fles
