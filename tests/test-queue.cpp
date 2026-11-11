
#include "../layer_linux/src/queue_synchronization.h"
#include <cassert>
#include <chrono>
#include <future>
using Q=dlssnr::QueueSynchronization<int>;
using namespace std::chrono_literals;
int main() {
    Q q;
    std::promise<void> release, owned;
    auto done=release.get_future().share();
    auto holder=std::async(std::launch::async,[&]{Q::QueueLease lock(q,1);owned.set_value();done.wait();});
    owned.get_future().wait();
    auto same=std::async(std::launch::async,[&]{Q::QueueLease lock(q,1);});
    auto other=std::async(std::launch::async,[&]{Q::QueueLease lock(q,2);});
    assert(other.wait_for(2s)==std::future_status::ready);
    assert(same.wait_for(20ms)==std::future_status::timeout);
    auto idle=std::async(std::launch::async,[&]{Q::IdleLease lock(q);});
    assert(idle.wait_for(20ms)==std::future_status::timeout);
    release.set_value();holder.get();same.get();idle.get();
    std::promise<void> idleOwned,idleRelease;
    auto idleDone=idleRelease.get_future().share();
    auto holdIdle=std::async(std::launch::async,[&]{Q::IdleLease lock(q);idleOwned.set_value();idleDone.wait();});
    idleOwned.get_future().wait();
    auto blocked1=std::async(std::launch::async,[&]{Q::QueueLease lock(q,1);});
    auto blocked2=std::async(std::launch::async,[&]{Q::QueueLease lock(q,2);});
    assert(blocked1.wait_for(20ms)==std::future_status::timeout);
    assert(blocked2.wait_for(20ms)==std::future_status::timeout);
    idleRelease.set_value();holdIdle.get();blocked1.get();blocked2.get();
    // Exceptions release both leases, allowing later lifecycle/queue operations.
    try { Q::QueueLease lock(q,3);throw 1; } catch(int) {}
    {Q::IdleLease lock(q);}
}
