#include "core/mvm_parallel_dispatch.h"

#include <iostream>

int main() {
    mvm::core::ParallelDispatchOrder parallel;
    if (!parallel.begin(100) || !parallel.requestA(101) || !parallel.requestB(102) ||
        !parallel.dispatchComplete(103) || !parallel.completionPoll() || !parallel.valid()) {
        std::cerr << "A dispatch -> B dispatch -> waitの順序を受理できません\n";
        return 1;
    }

    mvm::core::ParallelDispatchOrder serial;
    if (!serial.begin(100) || !serial.requestA(101)) {
        std::cerr << "negative対照群の準備に失敗しました\n";
        return 1;
    }
    if (serial.completionPoll() || serial.requestB(102) || serial.valid()) {
        std::cerr << "A dispatch -> wait A -> B dispatchを拒否できません\n";
        return 1;
    }

    std::cout << "parallel dispatch順序契約: PASS\n";
    return 0;
}
