#include "kcptun_client.h"
#include "snappy_stream.h"

kcptun_client::kcptun_client(asio::io_service &io_service,
                             asio::ip::tcp::endpoint local_endpoint,
                             asio::ip::udp::endpoint target_endpoint)
        : service_(io_service), socket_(io_service),
          target_endpoint_(target_endpoint), acceptor_(io_service, local_endpoint),
          local_(std::make_shared<Local>(io_service, target_endpoint)) {}

void kcptun_client::run() {
    if (!global_config.nocomp) {
        snappy_reader_ = std::make_shared<snappy_stream_reader>(service_,
                [this](char *buf, std::size_t len, Handler handler) {
                    snappy_stream_reader_output_handler(buf, len, handler);
                });
        snappy_writer_ = std::make_shared<snappy_stream_writer>(service_,
                [this](char *buf, std::size_t len, Handler handler) {
                    snappy_stream_writer_output_handler(buf, len, handler);
                });
    }
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    local_->run();
    smux_ = std::make_shared<smux>(
            service_, [this](char *buf, std::size_t len, Handler handler) {
                output_handler(buf, len, handler);
            });
    smux_->run();
    do_receive();
    do_accept();
}

void kcptun_client::do_accept() {
    auto self = shared_from_this();
    acceptor_.async_accept(socket_, [this, self](std::error_code ec) {
        TRACE
        if (ec) {
            TRACE
            return;
        }
        auto sock = std::make_shared<asio::ip::tcp::socket>(std::move(socket_));
        TRACE
        smux_->async_connect(
                [this, self, sock](std::shared_ptr<smux_sess> sess) {
                    TRACE
                    if (!sess) {
                        TRACE
                        return;
                    }
                    std::make_shared<kcptun_client_session>(service_, sock, sess)
                            ->run();
                });
        do_accept();
    });
}

void kcptun_client::do_receive() {
    auto self = shared_from_this();
    local_->async_read_some(
            buf_, sizeof(buf_), [this, self](std::error_code ec, std::size_t len) {
                if (ec) {
                    return;
                }
                if (snappy_reader_) {
                    snappy_reader_->async_input(buf_, len, [this, self](std::error_code ec, std::size_t) {
                        if (ec) {
                            return;
                        }
                        do_receive();
                    });
                } else {
                    smux_->input(buf_, len,
                            [this, self](std::error_code ec, std::size_t) {
                                TRACE
                                do_receive();
                            });
                }
            });
}

void kcptun_client::snappy_stream_writer_output_handler(char *buf, std::size_t len, Handler handler) {
    auto self = shared_from_this();
    local_->async_write(buf, len, [this, self, handler, len](std::error_code ec, std::size_t){
        if(handler) {
            handler(ec, len);
        }
    });
}

void kcptun_client::snappy_stream_reader_output_handler(char *buf, std::size_t len, Handler handler) {
    auto self = shared_from_this();
    smux_->input(buf, len, [this, self, handler, len](std::error_code ec, std::size_t){
        if(handler) {
            handler(ec, len);
        }
    });
}

void kcptun_client::output_handler(char *buf, std::size_t len,
                                   Handler handler) {
    tasks_.push_back(Task{buf, len, handler});
    if (!writing_) {
        writing_ = true;
        try_write_task();
    }
}

void kcptun_client::try_write_task() {
    auto self = shared_from_this();
    if (tasks_.empty()) {
        writing_ = false;
        return;
    }
    auto task = tasks_.front();
    tasks_.pop_front();
    auto write_handler = [this, self, task](std::error_code ec, std::size_t) {
        if (ec) {
            return;
        }
        if (task.handler) {
            task.handler(ec, task.len);
        }
        try_write_task();
    };
    if(snappy_writer_) {
        snappy_writer_->async_input(task.buf, task.len, write_handler);
    } else {
        local_->async_write(task.buf, task.len, write_handler);
    }
}

kcptun_client_session::kcptun_client_session(
        asio::io_service &io_service, std::shared_ptr<asio::ip::tcp::socket> sock,
        std::shared_ptr<smux_sess> sess)
        : service_(io_service), sock_(sock), sess_(sess) {}

void kcptun_client_session::run() {
    do_pipe1();
    do_pipe2();
}

void kcptun_client_session::do_pipe1() {
    auto self = shared_from_this();
    sock_->async_read_some(
            asio::buffer(buf1_, sizeof(buf1_)),
            [this, self](std::error_code ec, std::size_t len) {
                if (ec) {
                    sess_->destroy();
                    return;
                }
                sess_->async_write(buf1_, len,
                        [this, self](std::error_code ec, std::size_t) {
                            if (ec) {
                                sock_->cancel();
                                return;
                            }
                            do_pipe1();
                        });
            });
}

void kcptun_client_session::do_pipe2() {
    auto self = shared_from_this();
    sess_->async_read_some(
            buf2_, sizeof(buf2_),
            [this, self](std::error_code ec, std::size_t len) {
                if (ec) {
                    sock_->cancel();
                    return;
                }
                asio::async_write(
                        *sock_, asio::buffer(buf2_, len),
                        [this, self](std::error_code ec, std::size_t len) {
                            if (ec) {
                                sess_->destroy();
                                return;
                            }
                            do_pipe2();
                        });
            });
}