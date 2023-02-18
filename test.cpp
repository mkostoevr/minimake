#include "task.hpp"

#include <unistd.h>
#include <chrono>

#define TEST_LAYERS 4

int get_cpu_count() {
  return std::thread::hardware_concurrency();
}

size_t build_test_graph(BuildGraph &build_graph, int level, size_t& task_count) {
  std::vector<size_t> deps;
  for (size_t i = 0; i < level; i++) {
    deps.emplace_back(build_test_graph(build_graph, level - 1, task_count));
  }

  size_t t = build_graph.add_task([]{ std::cout << '.'; sleep(1); }, deps);
  task_count++;

  std::cout << "Task #" << t << " depends on: ";
  for (size_t dependency: deps) {
    std::cout << dependency << " ";
  }
  std::cout << '\n';

  return t;
}

size_t build_bad_graph(BuildGraph &build_graph, int level, size_t& task_count) {
  // Circuit:
  //   a -> b -> c -> d
  // but also:
  //   e -> f -> c -> e
  size_t a0 = build_graph.add_task([]{ std::cout << '.'; sleep(1); }, { 1 });
  size_t b1 = build_graph.add_task([]{ std::cout << '.'; sleep(1); }, { 2 });
  size_t c2 = build_graph.add_task([]{ std::cout << '.'; sleep(1); }, { 3, 4 });
  size_t d3 = build_graph.add_task([]{ std::cout << '.'; sleep(1); }, {});
  size_t e4 = build_graph.add_task([]{ std::cout << '.'; sleep(1); }, { 5 });
  size_t f5 = build_graph.add_task([]{ std::cout << '.'; sleep(1); }, { 2 });
  return a0;
}

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);

  BuildGraph build_graph;
  Builder builder(get_cpu_count());

  const size_t layers = TEST_LAYERS;
  size_t task_count = 0;
  size_t target_id = build_test_graph(build_graph, layers, task_count);
  //size_t target_id = build_bad_graph(build_graph, layers, task_count);

  std::cout << "Task count: " << task_count << '\n';

  auto start = std::chrono::system_clock::now();
  builder.execute(build_graph, target_id);
  auto finish = std::chrono::system_clock::now();

  auto start_seconds = std::chrono::time_point_cast<std::chrono::seconds>(start);
  auto finish_seconds = std::chrono::time_point_cast<std::chrono::seconds>(finish);
  auto time_seconds = finish_seconds - start_seconds;

  std::cout << "\nOverall time: " << time_seconds.count() << " seconds.\n";
}

