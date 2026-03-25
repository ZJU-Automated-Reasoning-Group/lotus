#pragma once
#include <string>
#include <vector>

/* const std::vector<std::string> inc_funcs = {"atomic_inc", "kref_get"}; */
const std::vector<std::string> inc_funcs = {"kref_get"};

const std::vector<std::string> init_funcs = {"atomic_set", "kref_init"};

/* const std::vector<std::string> dec_funcs = {"atomic_dec",
 * "atomic_dec_and_test", */
/*                                             "atomic_dec_and_lock", */
/*                                             "mutex_unlock", "kref_put"}; */

const std::vector<std::string> dec_funcs = {"kref_put"};
