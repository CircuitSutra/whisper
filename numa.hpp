#pragma once

// Attempt to use numactl to bind threads to CPUs with low load to increase
// peformance. If successful in finding adequate CPUs, this function will call
// exec() to invoke numactl with identical command-line arguments (except for
// --numa).
//
// Because exec() replaces the current process, this function only returns if
// unable to use numactl.
//
// This function is an adaptation of Evgeny Voevodin's python script
// NOLINTNEXTLINE(bugprone-reserved-identifier, cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
void attempt_numactl(int argc, char * argv[], unsigned cores, double max_load = 5.0);
