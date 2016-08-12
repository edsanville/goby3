#include <sys/types.h>
#include <sys/wait.h>

#include <deque>
#include <atomic>

#include "goby/common/logger.h"
#include "goby/sandbox/transport.h"
#include "test.pb.h"

#include <zmq.hpp>

// tests ZMQTransporter directly without InterThread

int publish_count = 0;
const int max_publish = 100;
int ipc_receive_count = {0};

std::atomic<bool> forward(true);
std::atomic<int> zmq_reqs(0);

using goby::glog;
using namespace goby::common::logger;

// parent process - thread 1
void publisher(const goby::protobuf::ZMQTransporterConfig& cfg)
{
    goby::ZMQTransporter<> zmq(cfg);

    double a = 0;
    while(publish_count < max_publish)
    {
        auto s1 = std::make_shared<Sample>();
        s1->set_a(a++);
        zmq.publish(s1, "Sample1");
        auto s2 = std::make_shared<Sample>();
        s2->set_a(s1->a() + 10);
        zmq.publish(s2, "Sample2");
        auto w1 = std::make_shared<Widget>();
        w1->set_b(s1->a() - 8);
        zmq.publish(w1, "Widget");

        glog.is(DEBUG1) && glog << "Published: " << publish_count << std::endl;
        usleep(1e3);
        ++publish_count;
    }    

    while(forward)
    {
        zmq.poll(std::chrono::milliseconds(100));
    }

}

// child process
void handle_sample1(const Sample& sample)
{
    glog.is(DEBUG1) && glog <<  "ZMQTransporter received publication sample1: " << sample.ShortDebugString() << std::endl;
    ++ipc_receive_count;
}

void handle_sample2(std::shared_ptr<const Sample> sample)
{
    glog.is(DEBUG1) && glog <<  "ZMQTransporter received publication sample2: " << sample->ShortDebugString() << std::endl;
    ++ipc_receive_count;
}


void handle_widget(const Widget& widget)
{
    glog.is(DEBUG1) && glog <<  "ZMQTransporter received publication widget: " << widget.ShortDebugString() << std::endl;
    ++ipc_receive_count;
}

void subscriber(const goby::protobuf::ZMQTransporterConfig& cfg)
{
    goby::ZMQTransporter<> zmq(cfg);
    zmq.subscribe<Sample>(&handle_sample1, "Sample1");
    zmq.subscribe<Sample>(&handle_sample2, "Sample2");
    zmq.subscribe<Widget>(&handle_widget, "Widget");
    while(ipc_receive_count < 3*max_publish)
    {
        zmq.poll();
    }
    
}

int main(int argc, char* argv[])
{
    goby::protobuf::ZMQTransporterConfig cfg;
    cfg.set_platform("test4");
    cfg.set_transport(goby::protobuf::ZMQTransporterConfig::TCP);
    cfg.set_ipv4_address("127.0.0.1");
    cfg.set_tcp_port(10005);
       
    pid_t child_pid = fork();

    bool is_child = (child_pid == 0);

    // goby::glog.add_stream(goby::common::logger::DEBUG3, &std::cerr);

    std::string os_name = std::string("/tmp/goby_test_sandbox4_") + (is_child ? "subscriber" : "publisher");
    std::ofstream os(os_name.c_str());
    goby::glog.add_stream(goby::common::logger::DEBUG3, &os);
    goby::glog.set_name(std::string(argv[0]) + (is_child ? "_subscriber" : "_publisher"));
    goby::glog.set_lock_action(goby::common::logger_lock::lock);                        

    std::unique_ptr<std::thread> t2, t3;
    std::unique_ptr<zmq::context_t> manager_context;
    std::unique_ptr<zmq::context_t> router_context;
    if(!is_child)
    {
        manager_context.reset(new zmq::context_t(1));
        router_context.reset(new zmq::context_t(1));

        goby::ZMQRouter router(*router_context, cfg);
        t2.reset(new std::thread([&] { router.run(); }));
        goby::ZMQManager manager(*manager_context, cfg, router);
        t3.reset(new std::thread([&] { manager.run(); }));
        sleep(1);
        std::thread t1([&] { publisher(cfg); });
        int wstatus;
        wait(&wstatus);
        forward = false;
        t1.join();
        router_context.reset();
        manager_context.reset();
        t2->join();
        t3->join();
        if(wstatus != 0) exit(EXIT_FAILURE);
    }
    else
    {
        std::thread t1([&] { subscriber(cfg); });
        t1.join();
    }

    glog.is(VERBOSE) && glog << (is_child ? "subscriber" : "publisher") << ": all tests passed" << std::endl;
    std::cout << (is_child ? "subscriber" : "publisher") << ": all tests passed" << std::endl;
}
