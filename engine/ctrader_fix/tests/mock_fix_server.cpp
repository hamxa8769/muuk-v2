#include "mock_fix_server.hpp"

#include <optional>
#include <stdexcept>

namespace muuk::ctrader_fix::test {

MockFixServer::MockFixServer()
    : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
    port_ = std::to_string(acceptor_.local_endpoint().port());
    acceptThread_ = std::thread([this] { acceptLoop(); });
}

MockFixServer::~MockFixServer() {
    running_ = false;
    asio::error_code ec;
    // Wake the accept thread first: on Linux, closing the acceptor does
    // not unblock a thread already inside a blocking accept() (on Windows
    // it does), so the join below would hang forever. Connecting once to
    // it makes accept() return; the loop then sees running_ == false and
    // exits. The extra connection is torn down with the rest below.
    {
        asio::ip::tcp::socket wake(io_);
        wake.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                                             static_cast<unsigned short>(std::stoi(port_))),
                     ec);
    }
    acceptor_.close(ec);
    {
        std::scoped_lock lock(connectionsMutex_);
        for (auto& conn : connections_) conn->socket.close(ec);
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    // Move the readers out before joining them: readLoop() takes
    // connectionsMutex_ first thing, so joining under that lock deadlocks
    // whenever a reader hasn't started yet (the wake connection's reader
    // usually hasn't) -- the join waits for a thread that waits for the lock.
    std::vector<std::thread> readers;
    {
        std::scoped_lock lock(connectionsMutex_);
        for (auto& conn : connections_) {
            if (conn->reader.joinable()) readers.push_back(std::move(conn->reader));
        }
    }
    for (auto& reader : readers) reader.join();
}

void MockFixServer::acceptLoop() {
    while (running_.load()) {
        auto conn = std::make_unique<Connection>(io_);
        asio::error_code ec;
        acceptor_.accept(conn->socket, ec);
        if (ec) return;  // acceptor closed during teardown, or a real error either way stop accepting

        std::size_t index;
        Connection* connPtr;
        {
            std::scoped_lock lock(connectionsMutex_);
            connections_.push_back(std::move(conn));
            index = connections_.size() - 1;
            connPtr = connections_[index].get();
        }
        connectionsCv_.notify_all();
        connPtr->reader = std::thread([this, index] { readLoop(index); });
    }
}

void MockFixServer::readLoop(std::size_t index) {
    Connection* conn;
    {
        std::scoped_lock lock(connectionsMutex_);
        conn = connections_[index].get();
    }

    char chunk[4096];
    while (running_.load()) {
        asio::error_code ec;
        const std::size_t n = conn->socket.read_some(asio::buffer(chunk), ec);
        if (ec) return;
        conn->readBuffer.append(chunk, n);

        while (true) {
            FixMessage msg("");
            std::size_t consumed = 0;
            if (!FixMessage::tryDecode(conn->readBuffer, msg, consumed)) break;
            conn->readBuffer.erase(0, consumed);

            std::scoped_lock lock(receivedMutex_);
            received_.push_back(ReceivedMessage{index, std::move(msg)});
            receivedCv_.notify_all();
        }
    }
}

FixMessage MockFixServer::waitForMessage(const std::string& msgType, std::size_t connectionIndex,
                                          std::chrono::milliseconds timeout) {
    std::unique_lock lock(receivedMutex_);
    std::optional<FixMessage> found;
    const bool ok = receivedCv_.wait_for(lock, timeout, [&] {
        // The oldest message of this type on this connection not returned
        // yet. Messages of other types/connections are left for their own
        // waits -- a single shared cursor used to skip (and lose) e.g. an
        // order that arrived on the trade connection just before the quote
        // subscription the test was waiting for on the quote connection.
        for (auto& r : received_) {
            if (!r.consumed && r.connectionIndex == connectionIndex && r.message.msgType() == msgType) {
                r.consumed = true;
                found = r.message;
                return true;
            }
        }
        return false;
    });
    if (!ok || !found) {
        throw std::runtime_error("MockFixServer: timed out waiting for MsgType=" + msgType + " on connection " +
                                  std::to_string(connectionIndex));
    }
    return *found;
}

void MockFixServer::sendToClient(std::size_t connectionIndex, const FixMessage& msg) {
    Connection* conn;
    {
        std::unique_lock lock(connectionsMutex_);
        connectionsCv_.wait_for(lock, std::chrono::seconds(2),
                                 [&] { return connectionIndex < connections_.size(); });
        if (connectionIndex >= connections_.size()) {
            throw std::runtime_error("MockFixServer: connection " + std::to_string(connectionIndex) +
                                      " not established yet");
        }
        conn = connections_[connectionIndex].get();
    }

    std::scoped_lock writeLock(conn->writeMutex);
    const std::string encoded = msg.encode();
    asio::error_code ec;
    asio::write(conn->socket, asio::buffer(encoded), ec);
    if (ec) {
        throw std::runtime_error("MockFixServer: write failed: " + ec.message());
    }
}

void MockFixServer::closeConnection(std::size_t connectionIndex) {
    Connection* conn;
    {
        std::scoped_lock lock(connectionsMutex_);
        if (connectionIndex >= connections_.size()) return;
        conn = connections_[connectionIndex].get();
    }
    asio::error_code ec;
    conn->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    conn->socket.close(ec);
}

}  // namespace muuk::ctrader_fix::test
