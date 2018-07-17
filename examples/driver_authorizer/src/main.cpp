#include <iostream>
#include <stdexcept>
#include <string>

#include <boost/program_options.hpp>

#include <components/run.hpp>

#include "component_list.hpp"

int main(int argc, char** argv) {
  namespace po = boost::program_options;

  po::variables_map vm;
  po::options_description desc("Allowed options");

  desc.add_options()("help,h", "produce this help message")(
      "config,c", po::value<std::string>()->default_value("config_dev.json"),
      "path to server config")("init-log,l", po::value<std::string>(),
                               "path to initialization log");

  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }

  if (vm.count("help")) {
    std::cerr << desc << '\n';
    return 0;
  }

  auto config_path = vm["config"].as<std::string>();
  std::string init_log_path;
  if (vm.count("init-log")) init_log_path = vm["init-log"].as<std::string>();

  try {
    components::Run(config_path, driver_authorizer::kComponentList,
                    init_log_path);
  } catch (const std::exception& ex) {
    std::cerr << "Server failed: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
