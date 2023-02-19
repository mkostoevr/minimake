#pragma once

#include <condition_variable>
#include <functional>
#include <iostream>
#include <cassert>
#include <thread>
#include <queue>
#include <mutex>
#include <set>

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

private:
    const size_t m_capacity;
    size_t m_size;
    std::vector<bool> m_bitset;
};

struct Target {
  size_t id;
  std::function<void()> task;
};

class ThreadUnsafeBuildGraph {
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

// I have never implemented thread pools myself so it's probably suboptimal
// and/or misses something.
//
// But I've done the best I could :)
class Pipeline {
private:
  struct WorkerSharedState {
    std::mutex task_queue_modification;
    std::condition_variable new_task_or_finish_request;
    std::queue<Target> task_queue;

    std::mutex finished_tasks_modification;
    std::condition_variable task_finished;
    TaskSet finished_tasks;

    bool pending_finish_request = false;

    explicit WorkerSharedState(size_t max_task_count)
        : finished_tasks(max_task_count) {}

    std::pair<Target, bool> request_next_task() {
      std::unique_lock lock(task_queue_modification);

      for (;;) {
        if (task_queue.size() > 0) {
          Target task = task_queue.front();
          task_queue.pop();
          // Return to the worker the task to execute.
          return std::make_pair(task, false);
        }
        
        if (pending_finish_request) {
          // Return to the worker a finish request.
          return std::make_pair((Target){}, true);
        }

        new_task_or_finish_request.wait(lock);
      }
    }

    void add_task(size_t id, std::function<void()> task) {
      std::lock_guard lock(task_queue_modification);

      assert(!pending_finish_request);
      if (pending_finish_request) {
        throw "Internal error: Attempt to add a task to finished pipeline.\n";
      }

      task_queue.push({ id, task });
      new_task_or_finish_request.notify_one();
    }

    void finish() {
      std::lock_guard lock(task_queue_modification);
      pending_finish_request = true;
      new_task_or_finish_request.notify_all();
    }

    void mark_task_as_finished(size_t task_id) {
      std::lock_guard lock(finished_tasks_modification);
      finished_tasks.insert(task_id);
      task_finished.notify_one();
    }

    bool task_is_finished(size_t task_id) {
      std::lock_guard lock(finished_tasks_modification);
      return finished_tasks.contains(task_id);
    }

    // Wait for "One task is finished since you checked last time, maybe you can
    // schedule new tasks now" event or "No currently running tasks" state from
    // the pipeline.
    //
    // Builder should save the number of built tasks since we have waited for
    // a task finish last time in order to prevent unneed waits in situations
    // like below:
    // 1. We have entered try_build_task.
    // 2. We checked that one dependency of one target is not ready yet.
    // 3. Then builder has finished building the dependency (so the target
    //    can be built now).
    // 4. And then we get out of try_build_task and start waiting when the
    //    builder will end one task, maybe he will end the task we was waiting
    //    for. The problem being the task we was waiting for is built while
    //    we was in try_build_task. So now we unusefully wait for one of other
    //    targets to build. They may be pretty big and it can waste a lot of
    //    time.
    //
    // To prevent such situation the builder not only should be able to inform
    // once one task is fininshed when we wait for it, but also should inform
    // if we have overslept some build fininshes since the last time we waited
    // for the next finished task.
    int last_checked_finished_tasks_count = 0;
    bool first_wait_till_can_try_build_again = true;

    void wait_till_can_try_build_again() {
      std::unique_lock lock(finished_tasks_modification);

      // If it's your first check if you can build anything, you can :)
      if (first_wait_till_can_try_build_again) {
        first_wait_till_can_try_build_again = false;
        return;
      }

      // Was there any more task finishes since the last check?
      // If was - you can try to build some target again, maybe
      // we have built its dependencies already.
      if (last_checked_finished_tasks_count != finished_tasks.size()) {
        last_checked_finished_tasks_count = finished_tasks.size();
        return;
      }

      // If there was no new task finished yet, let's wait for it.
      task_finished.wait(lock);
    }
  };

private:
  static void worker_thread(WorkerSharedState& shared_state) {
    for (;;) {
      auto [task, finish_request] = shared_state.request_next_task();
      if (finish_request) {
        return;
      }
      task.task();
      shared_state.mark_task_as_finished(task.id);
    }
  }

public:
  Pipeline(size_t num_threads, size_t max_task_count)
    : m_workers_shared_state(max_task_count) {
    for (size_t i = 0; i < num_threads; i++) {
      m_workers.emplace_back(worker_thread, std::ref(m_workers_shared_state));
    }
  }

  void schedule_task(size_t id, std::function<void()> task) {
    m_workers_shared_state.add_task(id, task);
  }

  bool task_is_finished(size_t task_id) {
    return m_workers_shared_state.task_is_finished(task_id);
  }

  void wait_till_can_try_build_again() {
    m_workers_shared_state.wait_till_can_try_build_again();
  }

  void finish() {
    // Signal to workers it's time to stop.
    m_workers_shared_state.finish();

    // Wait for workers to stop.
    for (std::thread& worker: m_workers) {
      worker.join();
    }
  }

private:
  std::vector<std::thread> m_workers;
  WorkerSharedState m_workers_shared_state;
};

class Builder {
private:
  static bool task_is_in_set(size_t task_id, const std::set<size_t>& the_set) {
    return the_set.find(task_id) != the_set.end();
  }

  void check_no_cyclic_deps4(const BuildGraph& build_graph,
                             size_t task_id,
                             std::set<size_t>& current_branch_tasks,
                             std::set<size_t>& analised_tasks) const {
    if (task_is_in_set(task_id, analised_tasks)) {
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
    auto [it, success] = current_branch_tasks.insert(task_id);

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
    // This could be optimized using something like a resizeable bit set
    // instead of a sorted tree. But I ran out of time :(
    std::set<size_t> current_branch_tasks;
    std::set<size_t> analised_tasks;

    check_no_cyclic_deps4(build_graph,
                          task_id,
                          current_branch_tasks,
                          analised_tasks);
  }

  void schedule_task(size_t task_id, std::function<void()> task) {
    m_pipeline.schedule_task(task_id, task);
  }

  // Returns true if the task is finished already, false othervice.
  bool try_build_task(const BuildGraph& build_graph,
                      size_t task_id,
                      std::set<size_t>& waiting_tasks,
                      std::set<size_t>& scheduled_tasks) {
    // If our task is done - return true.
    if (m_pipeline.task_is_finished(task_id)) {
      return true;
    }

    // If the task is shcheduled already, skip it. It's going to be built later.
    if (task_is_in_set(task_id, scheduled_tasks)) {
      return false;
    }

    // If our task is in waiting_tasks then we already analised it in one of
    // other branches. No need to check our dependencies again, we're
    // still waiting.
    if (task_is_in_set(task_id, waiting_tasks)) {
      return false;
    }

    // Try to build all our dependencies.
    const std::vector<size_t>& deps = build_graph.get_task_deps(task_id);
    bool all_dependencies_are_built = true;
    for (size_t dependency: deps) {
      all_dependencies_are_built &= try_build_task(build_graph,
                                                   dependency,
                                                   waiting_tasks,
                                                   scheduled_tasks);
    }

    if (all_dependencies_are_built) {
      // The task is ready to be built, let's schedule it.
      schedule_task(task_id, build_graph.get_task(task_id));
      scheduled_tasks.insert(task_id);
    } else {
      // Need to wait until our dependencies are built.
      waiting_tasks.insert(task_id);
    }
    return false;
  }

  void build(const BuildGraph& build_graph, size_t task_id) {
    // This could be optimized the same way, using bit set.
    // Does it worth it?
    std::set<size_t> waiting_tasks;
    std::set<size_t> scheduled_tasks;

    do {
      m_pipeline.wait_till_can_try_build_again();
      waiting_tasks.clear();
      try_build_task(build_graph,
                     task_id,
                     waiting_tasks,
                     scheduled_tasks);
    } while (waiting_tasks.size() > 0);
    m_pipeline.finish();
  }

public:
  explicit Builder(size_t num_threads, const BuildGraph& build_graph)
      : m_pipeline(num_threads, build_graph.size())
      , m_build_graph(build_graph) {}

  void execute(size_t task_id) {
    std::cout << "Cyclic dependency check... ";
    check_no_cyclic_deps(m_build_graph, task_id);
    std::cout << "Done.\n";

    std::cout << "Build... ";
    build(m_build_graph, task_id);
    std::cout << "Done.\n";
  }

private:
  Pipeline m_pipeline;
  const BuildGraph& m_build_graph;
};

