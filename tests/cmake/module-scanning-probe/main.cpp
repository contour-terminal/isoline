// SPDX-License-Identifier: Apache-2.0
//
// Imports the probe's module: building it needs CMake's module dependency scanning.
import probe;

int main()
{
    return answer() == 42 ? 0 : 1;
}
