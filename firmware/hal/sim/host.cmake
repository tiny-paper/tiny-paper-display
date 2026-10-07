# The display repository carries one file of the simulator's HAL: its JSON reader (src/sim_json.c), with which the
# host tests load docs/spec/vectors. No simulator: TINY_HAVE_SIM_HAL stays unset.
# SPDX-License-Identifier: GPL-3.0-or-later
target_include_directories(tiny_hal_sim PUBLIC "${TINY_FW}/hal/sim/include")
