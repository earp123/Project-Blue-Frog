/*
 * Force-included into every vendored Codec 2 source (CMakeLists.txt): build
 * the codec at -O2 whatever the global optimisation level. Zephyr adds its
 * -Os after the target's own options, so a plain -O2 on the target loses.
 * A pragma applies regardless of flag order, and leaves the vendored sources
 * untouched.
 */
#pragma GCC optimize("O2")
