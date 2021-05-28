#include <faabric/transport/MessageEndpoint.h>

#include <faabric/util/gids.h>
#include <unistd.h>

namespace faabric::transport {
MessageEndpoint::MessageEndpoint(const std::string& hostIn, int portIn)
  : host(hostIn)
  , port(portIn)
  , tid(std::this_thread::get_id())
  , id(faabric::util::generateGid())
{}

MessageEndpoint::~MessageEndpoint()
{
    if (this->socket != nullptr) {
        faabric::util::getLogger()->warn(
          "Destroying an open message endpoint!");
        this->close(false);
    }
}

void MessageEndpoint::open(faabric::transport::MessageContext& context,
                           faabric::transport::SocketType sockType,
                           bool bind)
{
    // Check we are opening from the same thread. We assert not to incur in
    // costly checks when running a Release build.
    assert(tid == std::this_thread::get_id());

    const auto& logger = faabric::util::getLogger();

    std::string address =
      "tcp://" + this->host + ":" + std::to_string(this->port);

    // Note - only one socket may bind, but several can connect. This
    // allows for easy N - 1 or 1 - N PUSH/PULL patterns. Order between
    // bind and connect does not matter.
    switch (sockType) {
        case faabric::transport::SocketType::PUSH:
            try {
                this->socket = std::make_unique<zmq::socket_t>(
                  context.get(), zmq::socket_type::push);
            } catch (zmq::error_t& e) {
                logger->error(
                  "Error opening SEND socket to {}: {}", address, e.what());
                throw;
            }
            break;
        case faabric::transport::SocketType::PULL:
            try {
                this->socket = std::make_unique<zmq::socket_t>(
                  context.get(), zmq::socket_type::pull);
            } catch (zmq::error_t& e) {
                logger->error("Error opening RECV socket bound to {}: {}",
                              address,
                              e.what());
                throw;
            }
            break;
        default:
            throw std::runtime_error("Unrecognized socket type");
    }
    assert(this->socket != nullptr);

    // Bind or connect the socket
    if (bind) {
        try {
            this->socket->bind(address);
        } catch (zmq::error_t& e) {
            logger->error("Error binding socket to {}: {}", address, e.what());
            throw;
        }
    } else {
        try {
            this->socket->connect(address);
        } catch (zmq::error_t& e) {
            logger->error(
              "Error connecting socket to {}: {}", address, e.what());
            throw;
        }
    }
}

void MessageEndpoint::send(uint8_t* serialisedMsg, size_t msgSize, bool more)
{
    assert(tid == std::this_thread::get_id());
    assert(this->socket != nullptr);

    const auto& logger = faabric::util::getLogger();

    if (more) {
        try {
            auto res = this->socket->send(zmq::buffer(serialisedMsg, msgSize),
                                          zmq::send_flags::sndmore);
            if (res != msgSize) {
                logger->error("Sent different bytes than expected (sent "
                              "{}, expected {})",
                              res.value_or(0),
                              msgSize);
                throw std::runtime_error("Error sending message");
            }
        } catch (zmq::error_t& e) {
            logger->error("Error sending message: {}", e.what());
            throw;
        }
    } else {
        try {
            auto res = this->socket->send(zmq::buffer(serialisedMsg, msgSize),
                                          zmq::send_flags::none);
            if (res != msgSize) {
                logger->error("Sent different bytes than expected (sent "
                              "{}, expected {})",
                              res.value_or(0),
                              msgSize);
                throw std::runtime_error("Error sending message");
            }
        } catch (zmq::error_t& e) {
            logger->error("Error sending message: {}", e.what());
            throw;
        }
    }
}

// By passing the expected recv buffer size, we instrument zeromq to receive on
// our provisioned buffer
Message MessageEndpoint::recv(int size)
{
    assert(tid == std::this_thread::get_id());
    assert(this->socket != nullptr);
    assert(size >= 0);

    const auto& logger = faabric::util::getLogger();

    // Pre-allocate buffer to avoid copying data
    if (size > 0) {
        Message msg(size);

        try {
            auto res = this->socket->recv(zmq::buffer(msg.udata(), msg.size()));
            if (res.has_value() && (res->size != res->untruncated_size)) {
                logger->error("Received more bytes than buffer can hold. "
                              "Received: {}, capacity {}",
                              res->untruncated_size,
                              res->size);
                throw std::runtime_error("Error receiving message");
            }
        } catch (zmq::error_t& e) {
            if (e.num() == ZMQ_ETERM) {
                // Return empty message to signify termination
                logger->trace("Shutting endpoint down after receiving ETERM");
                return Message();
            } else {
                logger->error("Error receiving message: {}", e.what());
                throw;
            }
        }

        return msg;
    }

    // Allocate a message to receive data
    zmq::message_t msg;
    try {
        auto res = this->socket->recv(msg);
        if (!res.has_value()) {
            logger->error("Error receiving message: EAGAIN");
            throw std::runtime_error("Error receiving message");
        }
    } catch (zmq::error_t& e) {
        if (e.num() == ZMQ_ETERM) {
            // Return empty message to signify termination
            logger->trace("Shutting endpoint down after receiving ETERM");
            return Message();
        } else {
            logger->error("Error receiving message: {}", e.what());
            throw;
        }
    }

    // Copy the received message to a buffer whose scope we control
    return Message(msg);
}

void MessageEndpoint::close(bool bind)
{
    if (this->socket != nullptr) {
        const auto& logger = faabric::util::getLogger();

        if (tid != std::this_thread::get_id()) {
            logger->warn("Closing socket from a different thread");
        }

        std::string address =
          "tcp://" + this->host + ":" + std::to_string(this->port);

        // We duplicate the call to close() because when unbinding, we want to
        // block until we _actually_ have unbinded, i.e. 0MQ has closed the
        // socket (which happens asynchronously). For connect()-ed sockets we
        // don't care.
        // Not blobking on un-bind can cause race-conditions when the underlying
        // system is slow at closing sockets, and the application relies a lot
        // on synchronous message-passing.
        if (bind) {
            try {
                this->socket->unbind(address);
            } catch (zmq::error_t& e) {
                if (e.num() != ZMQ_ETERM) {
                    logger->error("Error unbinding socket: {}", e.what());
                    throw;
                }
            }
            // NOTE - unbinding a socket has a considerable overhead compared to
            // disconnecting it.
            // TODO - could we reuse the monitor?
            try {
                {
                    zmq::monitor_t mon;
                    const std::string monAddr =
                      "inproc://monitor_" + std::to_string(id);
                    mon.init(*(this->socket), monAddr, ZMQ_EVENT_CLOSED);
                    this->socket->close();
                    mon.check_event(-1);
                }
            } catch (zmq::error_t& e) {
                if (e.num() != ZMQ_ETERM) {
                    logger->error("Error closing bind socket: {}", e.what());
                    throw;
                }
            }
        } else {
            try {
                this->socket->disconnect(address);
            } catch (zmq::error_t& e) {
                if (e.num() != ZMQ_ETERM) {
                    logger->error("Error disconnecting socket: {}", e.what());
                    throw;
                }
            }
            try {
                this->socket->close();
            } catch (zmq::error_t& e) {
                logger->error("Error closing connect socket: {}", e.what());
                throw;
            }
        }

        // Finally, null the socket
        this->socket = nullptr;
    }
}

std::string MessageEndpoint::getHost()
{
    return host;
}

int MessageEndpoint::getPort()
{
    return port;
}

/* Send and Recv Message Endpoints */

SendMessageEndpoint::SendMessageEndpoint(const std::string& hostIn, int portIn)
  : MessageEndpoint(hostIn, portIn)
{}

void SendMessageEndpoint::open(MessageContext& context)
{
    faabric::util::getLogger()->trace(
      fmt::format("Opening socket: {} (SEND {}:{})", id, host, port));

    MessageEndpoint::open(context, SocketType::PUSH, false);
}

void SendMessageEndpoint::close()
{
    faabric::util::getLogger()->trace(
      fmt::format("Closing socket: {} (SEND {}:{})", id, host, port));

    MessageEndpoint::close(false);
}

RecvMessageEndpoint::RecvMessageEndpoint(int portIn)
  : MessageEndpoint(ANY_HOST, portIn)
{}

void RecvMessageEndpoint::open(MessageContext& context)
{
    faabric::util::getLogger()->trace(
      fmt::format("Opening socket: {} (RECV {}:{})", id, ANY_HOST, port));

    MessageEndpoint::open(context, SocketType::PULL, true);
}

void RecvMessageEndpoint::close()
{
    faabric::util::getLogger()->trace(
      fmt::format("Closing socket: {} (RECV {}:{})", id, ANY_HOST, port));

    MessageEndpoint::close(true);
}
}