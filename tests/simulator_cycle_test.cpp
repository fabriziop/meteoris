#include "sim/signal_cycle.hpp"

#include <cassert>

int main()
{
    using meteoris::sim::cycleAmplitude;
    using meteoris::sim::cycleGeneratesSignals;

    assert(cycleGeneratesSignals(0, 3));
    assert(cycleGeneratesSignals(2, 3));
    assert(!cycleGeneratesSignals(3, 3));
    assert(cycleGeneratesSignals(1000, 0));

    assert(cycleAmplitude(6.0, 1.0, 0) == 6.0);
    assert(cycleAmplitude(6.0, 1.0, 2) == 4.0);
    assert(cycleAmplitude(1.0, 1.0, 2) == 0.0);
    assert(cycleAmplitude(3.0, 0.0, 50) == 3.0);
    return 0;
}
