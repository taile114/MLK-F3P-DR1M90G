// Board-only integration test. Stop ps-local first: this temporarily owns U7.
#include "pl_host.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <cmath>
using Clock=std::chrono::steady_clock;
static int fd=-1;
static void finish() { if(fd>=0) { pl_set_alarm(fd,0); pl_close(fd); } }
static void require(bool ok,const char *what) {
    if(!ok) { std::fprintf(stderr,"FAIL %s\n",what); std::exit(1); }
}
static unsigned status() {
    uint32_t state=0,s=0;
    require(pl_read_alarm(fd,&state,&s)==0,"read alarm"); return s;
}
static void set(unsigned state) { require(pl_set_alarm(fd,state)==0,"write alarm"); }
static double seconds(Clock::time_point t) { return std::chrono::duration<double>(Clock::now()-t).count(); }
static void measure(unsigned state,int pulses,double high,double total) {
    set(0); std::this_thread::sleep_for(std::chrono::milliseconds(1100)); set(state);
    auto start=Clock::now(),last=start;
    bool previous=false;
    std::vector<double> widths,gaps;
    double down=0;
    while(seconds(start)<total) {
        set(state); // repeated state renews lease without retrigger
        const bool pin=status()&1;
        if(pin!=previous) {
            const double time=seconds(start);
            if(pin) { if(down) gaps.push_back(time-down); last=Clock::now(); }
            else { widths.push_back(seconds(last)); down=time; }
            std::printf("edge state=0x%02x pin=%u t=%.4f\n",state,unsigned(pin),time);
            previous=pin;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!previous && int(widths.size())==pulses,"pulse count/complete");
    for(auto width:widths) require(std::abs(width-high)<0.10,"high duration");
    for(auto gap:gaps) require(std::abs(gap-1.0)<0.10,"gap duration");
    std::printf("PASS state=0x%02x pulses=%d high=%.1f\n",state,pulses,high);
}
int main(int argc,char **argv) {
    if(argc!=2 || std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"usage: test_alarm_hardware --run (stop ps-local first)\n"); return 2;
    }
    fd=pl_open(nullptr); require(fd>=0,"open device"); std::atexit(finish);
    for(int ch=0;ch<4;++ch) measure(1u<<ch,ch+1,1,2*(ch+1)-0.7);
    for(int grade=1;grade<=3;++grade) measure(unsigned(grade)<<4,1,3*grade,3*grade+0.3);
    set(0); std::this_thread::sleep_for(std::chrono::milliseconds(1100)); set(8);
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); set(0x18);
    auto start=Clock::now();
    while(seconds(start)<2.85) { set(0x18); require((status()&7)==7,"model preemption remains continuous");
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    set(0); require((status()&1)==0,"healthy immediately low");
    set(0x30); std::this_thread::sleep_for(std::chrono::milliseconds(2150));
    require((status()&9)==8,"PS heartbeat expiry forces low");
    std::puts("PASS hardware alarm patterns, preemption, clear, lease");
}
