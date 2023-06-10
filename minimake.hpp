#pragma once

#include <condition_variable>
#include <functional>
#include <iostream>
#include <cassert>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>

class TaskSet {
public:
    explicit TaskSet(size_t max_task_count)
        : m_capacity(max_task_count)
        , m_bitset(max_task_count, 0)
        , m_size(0) {}

    /*
     * Adds a task to the set.
     *
     * @return true if task is added successfully,
     *         false if it already exists.
     */
    bool insert(size_t task_id) {
        assert(task_id < m_capacity);

        if (m_bitset[task_id]) {
            return false;
        }

        m_bitset[task_id] = true;
        m_size++;
        return true;
    }

    /*
     *  Remove a task from the set.
     *
     *  @warning If the task does not exist in
     *           the set, behavior is undefined.
     */
    void erase(size_t task_id) {
        assert(m_bitset[task_id]);

        m_bitset[task_id] = false;
        m_size--;
    }

    /*
     * Check whether the task exists in the set.
     */
    bool contains(size_t task_id) {
        assert(task_id < m_capacity);

        return m_bitset[task_id];
    }

    /*
     * Count existing tasks in the set.
     */
    size_t size() {
        return m_size;
    }

    /*
     * Reset the task set.
     */
    void clear() {
        std::fill(m_bitset.begin(), m_bitset.end(), false);
        m_size = 0;
    }

private:
    const size_t m_capacity;
    size_t m_size;
    std::vector<bool> m_bitset;
};

struct Target {
  size_t id;
  std::function<void()> task;

  operator bool() {
    assert((id == SIZE_MAX && task == nullptr)
        || (id != SIZE_MAX && task != nullptr));
    return id != SIZE_MAX;
  }
};

class ReversedBuildGraph;

class ThreadUnsafeBuildGraph {
  friend ReversedBuildGraph;

private:
  std::vector<std::function<void()>> m_tasks;
  std::vector<std::vector<size_t>> m_task_deps;

public:
  size_t add_task(std::function<void()> task, std::vector<size_t> deps) {
    size_t task_id = m_tasks.size();
    m_tasks.push_back(task);
    m_task_deps.push_back(deps);
    return task_id;
  }

  std::function<void()> get_task(size_t task_id) const {
    assert(m_tasks.size() > task_id);
    return m_tasks[task_id];
  }

  const std::vector<size_t>& get_task_deps(size_t task_id) const {
    assert(m_task_deps.size() > task_id);
    return m_task_deps[task_id];
  }

  size_t size() const noexcept {
    return m_tasks.size();
  }
};

class BuildGraph: ThreadUnsafeBuildGraph {
  friend ReversedBuildGraph;

private:
  // Now if you start reading this graph from many threads we have this mutex
  // constantly changing. It contains a bunch of variables and these changes
  // require all the CPUs synchronize their caches to always have up-to-date
  // state of the lock.
  //
  // You don't need the lock if no one is going to write the graph anymore, so we
  // can go to read-only mode where we don't touch the mutex and read data in
  // thread-unsafe manner. So we will prevent overhead on CPU cache coherency.
  //
  // But this is going to make the code uglier and is not required for the build
  // system, since the graph is only read from one thread. I made it thread-safe
  // just to show I know stuff :)
  mutable std::mutex m_lock;

public:
  // The thing is: you simply can not introduce a cyclic dpendency with this API xD.
  // Well, without cheating of course.
  size_t add_task(std::function<void()> task, std::vector<size_t> deps) {
    std::lock_guard lock(m_lock);
    return ThreadUnsafeBuildGraph::add_task(task, deps);
  }

  std::function<void()> get_task(size_t task_id) const {
    std::lock_guard lock(m_lock);
    return ThreadUnsafeBuildGraph::get_task(task_id);
  }

  const std::vector<size_t>& get_task_deps(size_t task_id) const {
    std::lock_guard lock(m_lock);
    return ThreadUnsafeBuildGraph::get_task_deps(task_id);
  }

  size_t size() const noexcept {
    std::lock_guard lock(m_lock);
    return ThreadUnsafeBuildGraph::size();
  }
};

class ReversedBuildGraph {
public:
  ReversedBuildGraph(
    const BuildGraph& build_graph,
    size_t task_id,
    size_t num_threads
  ) : m_tasks(build_graph.m_tasks)
    , m_task_dependers(build_graph.size())
    , m_task_dependencies_remained(build_graph.size())
    , m_independent_tasks_i(num_threads)
    , m_independent_tasks(num_threads) {
    // Reserve space so the vector won't reallocate on new task addition.
    // TODO: Use a bufset instead of the reallocatable vector.
    for (size_t i = 0; i < num_threads; i++) {
      m_independent_tasks[i].reserve(build_graph.size());
    }
    size_t next_thread_id = 0;
    fill_data(build_graph, task_id, num_threads, next_thread_id);
  }

  std::function<void()> get_task(size_t task_id) const {
    assert(task_id != SIZE_MAX);
    assert(m_tasks[task_id] != nullptr);
    return m_tasks[task_id];
  }

  Target get_next_target(Target last_target, size_t thread_id) {
    // TODO: Use something non-allocating here?
    std::vector<size_t> got_ready = finish_target(last_target.id);
    if (got_ready.size() != 0) {
      // TODO: Put second and next tasks into the m_independent_tasks.
      assert(got_ready.size() == 1);
      return { got_ready[0], get_task(got_ready[0]) };
    }
    // If we can't continue the tree now, let's look for the next one.
    if (size_t task_id = aquire_next_independent_task(thread_id); task_id != SIZE_MAX) {
      return { task_id, get_task(task_id) };
    }
    // No tasks to execute.
    return { SIZE_MAX, nullptr };
  }

private:
  __attribute__((noinline))
  size_t aquire_next_independent_task(size_t thread_id) {
    while (m_independent_tasks_i[thread_id] < m_independent_tasks[thread_id].size()) {
      size_t expected = m_independent_tasks_i[thread_id];
      bool ok = std::atomic_compare_exchange_weak(
        &m_independent_tasks_i[thread_id],
        &expected,
        expected + 1
      );
      if (ok) {
        return m_independent_tasks[thread_id][expected];
      }
    }
    // TODO: Wait for a new independent task.
    return SIZE_MAX;
  }

  __attribute__((noinline))
  std::vector<size_t> finish_target(size_t last_task_id) {
    // The dependers that became ready after the task is finished.
    if (last_task_id == SIZE_MAX) {
      // No task is finished, so no depender got ready.
      return {};
    }
    std::vector<size_t> result;
    for (size_t depender: m_task_dependers[last_task_id]) {
      if (aquire_depender(depender)) {
        result.push_back(depender);
      }
    }
    return result;
  }

  // Decrement the task's remained dependency count.
  // @param   task_id The depender ID.
  // @returns true    If the dependency count was zerified
  //                  (so now the task can be executed).
  //          false   Othervice.
  __attribute__((noinline))
  bool aquire_depender(size_t task_id) {
    std::atomic<size_t>& dep_n = m_task_dependencies_remained[task_id];
    size_t expected = SIZE_MAX;
    for (;;) {
      expected = dep_n;
      if (std::atomic_compare_exchange_weak(&dep_n, &expected, expected - 1)) {
        break;
      }
    }
    if (expected == 1) {
      // We have satisfied the last dependency of the target.
      return true;
    }
    return false;
  }

  void fill_data(
    const BuildGraph& build_graph,
    size_t task_id,
    size_t num_threads,
    size_t &next_thread_id
  ) noexcept {
    const std::vector<size_t>& deps = build_graph.get_task_deps(task_id);

    if (deps.size() == 0) {
      // This task is ready to be put in a queue.
      m_independent_tasks[next_thread_id++].push_back(task_id);
      if (next_thread_id == num_threads) {
        next_thread_id = 0;
      }
    } else {
      // This task is ready when all of its dpendencies are ready.
      m_task_dependencies_remained[task_id] = deps.size();

      for (size_t dep_id: deps) {
        // This task is a depender of its dependency,
        // because it depends in its dependency.
        m_task_dependers[dep_id].push_back(task_id);

        // Go analise our dependencies.
        fill_data(build_graph, dep_id, num_threads, next_thread_id);
      }
    }
  }

private:
  // Overall amount of tasks to execute.
  std::vector<std::function<void()>> m_tasks;
  std::vector<std::vector<size_t>> m_task_dependers;
  // TODO: Measure with uint32_t.
  std::vector<std::atomic<size_t>> m_task_dependencies_remained;
  std::vector<std::vector<size_t>> m_independent_tasks;
  std::vector<std::atomic<size_t>> m_independent_tasks_i;
};

// I have never implemented thread pools myself so it's probably suboptimal
// and/or misses something.
//
// But I've done the best I could :)
class Pipeline {
private:
  struct WorkerSharedState {
    ReversedBuildGraph &m_rbg;

    explicit WorkerSharedState(size_t max_task_count, ReversedBuildGraph &rbg)
      : m_rbg { rbg }
      {}

    Target get_next_target(Target last_target, size_t thread_id) {
      return m_rbg.get_next_target(last_target, thread_id);
    }
  };

  // TODO: Align to cache line size to prevent false sharing.
  struct WorkerState {
    size_t m_id;
    WorkerSharedState &m_shared_state;
    Target m_last_target;

    WorkerState(WorkerSharedState &shared_state, size_t id)
      : m_id { id }
      , m_shared_state { shared_state }
      , m_last_target { SIZE_MAX }
      {}

    std::pair<Target, bool> get_next_task() {
      for (;;) {
        if (
          Target target = m_shared_state.get_next_target(m_last_target, m_id);
          target
        ) {
          m_last_target = target;
          return std::make_pair(target, false);
        }

        return std::make_pair((Target){}, true);
      }
    }
  };

private:
  static void worker_thread(std::shared_ptr<WorkerState> state,
                            WorkerSharedState& shared_state) {
    for (;;) {
      auto [task, finish_request] = state->get_next_task();
      if (finish_request) {
        return;
      }
      task.task();
    }
  }

public:
  Pipeline(size_t num_threads, size_t max_task_count, ReversedBuildGraph &rbg)
    : m_workers_shared_state(max_task_count, rbg)
    {
    for (size_t i = 0; i < num_threads; i++) {
      auto worker_state = m_worker_states.emplace_back(
        new WorkerState(m_workers_shared_state, i)
      );
      m_workers.emplace_back(worker_thread,
                             worker_state,
                             std::ref(m_workers_shared_state));
    }
  }

  void wait() {
    for (std::thread& worker: m_workers) {
      worker.join();
    }
  }

private:
  std::vector<std::thread> m_workers;
  std::vector<std::shared_ptr<WorkerState>> m_worker_states;
  WorkerSharedState m_workers_shared_state;
  size_t m_next_worker = 0;
};

class Builder {
private:
  void check_no_cyclic_deps4(const BuildGraph& build_graph,
                             size_t task_id,
                             TaskSet& current_branch_tasks,
                             TaskSet& analised_tasks) const {
    if (analised_tasks.contains(task_id)) {
      // This task was already analised in another dependency graph branch.
      //
      // Now we know that the dependency subgraph starting from this task_id
      // is not cyclic, cause othervice it would not pass check in the first
      // branch the task occured in.
      //
      // Checkout this:
      //   a -> b -> c -> d
      //   &&
      //   e -> f -> c -> e (cyclic, should be detected in a -> branch).
      return;
    }

    // Put ourselves into current branch task set.
    bool success = current_branch_tasks.insert(task_id);

    // If we was in the branch already, that means we have a cyclic dependency,
    // and it's not a branch at all, it's a circuit :)
    if (!success) {
      std::cerr << "Cyclic dependency! No time = no additional info :)\n";
      throw "Cyclic dependence detected";
    }

    // Get our depepdencies.
    const std::vector<size_t>& deps = build_graph.get_task_deps(task_id);

    // Test if dependencies also don't make circuit.
    for (size_t dependency_id: deps) {
      check_no_cyclic_deps4(build_graph,
                            dependency_id,
                            current_branch_tasks,
                            analised_tasks);
    }

    // Successfully analised the task: no cyclic dependencies found in its subgraph.
    analised_tasks.insert(task_id);

    // We are not one of tasks in the branch anymore.
    current_branch_tasks.erase(task_id);
  }

  void check_no_cyclic_deps(const BuildGraph& build_graph, size_t task_id) const {
    TaskSet current_branch_tasks(build_graph.size());
    TaskSet analised_tasks(build_graph.size());

    check_no_cyclic_deps4(build_graph,
                          task_id,
                          current_branch_tasks,
                          analised_tasks);
  }

  void build(ReversedBuildGraph& rbg, size_t task_id) {
    // TODO: Get rid of the m_build_graph.size().
    Pipeline pipeline { m_num_threads, m_build_graph.size(), rbg };

    // TODO: Let it just locklessly take tasks from the end himself.
    // Like run_on_rbg().
    pipeline.wait();
  }

public:
  explicit Builder(size_t num_threads, const BuildGraph& build_graph)
    : m_num_threads { num_threads }
    , m_build_graph { build_graph }
    {}

  void execute(size_t task_id) {
    std::cout << "Cyclic dependency check... ";
    check_no_cyclic_deps(m_build_graph, task_id);
    std::cout << "Done." << std::endl;

    std::cout << "Reversing graph... ";
    ReversedBuildGraph reversed_build_graph(m_build_graph, task_id, m_num_threads);
    std::cout << "Done." << std::endl;

    std::cout << "Build... ";
    build(reversed_build_graph, task_id);
    std::cout << "Done." << std::endl;
  }

private:
  const size_t m_num_threads;
  const BuildGraph& m_build_graph;
};

