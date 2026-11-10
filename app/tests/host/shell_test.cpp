/* Navi5 — generic shell helpers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/json_num.h"
#include "ui/anim.h"
#include <cassert>
#include <limits>

int main()
{
    using settings::to_int;
    assert(to_int<int>(std::numeric_limits<double>::quiet_NaN(), 7) == 7);
    assert(to_int<int>(std::numeric_limits<double>::infinity()) == std::numeric_limits<int>::max());
    assert(to_int<int>(-std::numeric_limits<double>::infinity()) == std::numeric_limits<int>::min());
    assert(to_int<int>(2.9) == 2);
    ui::Anim full, halves;
    full.to(1); halves.to(1);
    full.step(0.02f); halves.step(0.01f); halves.step(0.01f);
    assert(std::fabs(full.value - halves.value) < 0.00001f);
    for (int i = 0; i < 200; ++i) full.step(0.02f);
    assert(!full.step(0.02f) && full.value == 1);
    assert(ui::smoothstep(-1) == 0 && ui::smoothstep(2) == 1);
}
