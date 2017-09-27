// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "task_runner.h"
#include <vespa/vespalib/util/lambdatask.h>
#include <vespa/vespalib/util/threadstackexecutor.h>
#include <future>

using vespalib::makeLambdaTask;

namespace proton::initializer {

TaskRunner::TaskRunner(vespalib::Executor &executor)
    : _executor(executor),
      _runningTasks(0u)
{
}

TaskRunner::~TaskRunner()
{
    assert(_runningTasks == 0u);
}

void
TaskRunner::getReadyTasks(const InitializerTask::SP task, TaskList &readyTasks, TaskSet &checked)
{
    if (task->getState() != State::BLOCKED) {
        return; // task running or done, all dependencies done
    }
    if (!checked.insert(task.get()).second) {
        return; // task already checked from another depender
    }
    const TaskList &deps = task->getDependencies();
    bool ready = true;
    for (const auto &dep : deps) {
        switch (dep->getState()) {
        case State::RUNNING:
            ready = false;
            break;
        case State::DONE:
            break;
        case State::BLOCKED:
            ready = false;
            getReadyTasks(dep, readyTasks, checked);
        }
    }
    if (ready) {
        readyTasks.push_back(task);
    }
}

void
TaskRunner::setTaskRunning(InitializerTask &task)
{
    // run by context executor
    task.setRunning();
    ++_runningTasks;
}

void
TaskRunner::setTaskDone(InitializerTask &task, Context::SP context)
{
    // run by context executor
    task.setDone();
    --_runningTasks;
    pollTask(context);
}

void
TaskRunner::internalRunTask(InitializerTask::SP task, Context::SP context)
{
    // run by context executor
    assert(task->getState() == State::BLOCKED);
    setTaskRunning(*task);
    auto done(makeLambdaTask([=]() { setTaskDone(*task, context); }));
    _executor.execute(makeLambdaTask([=, done(std::move(done))]() mutable
                                     {   task->run();
                                         context->execute(std::move(done)); }));
}

void
TaskRunner::internalRunTasks(const TaskList &taskList, Context::SP context)
{
    // run by context executor
    for (auto &task : taskList) {
        internalRunTask(task, context);
    }
}

void
TaskRunner::runTask(InitializerTask::SP task)
{
    vespalib::ThreadStackExecutor executor(1, 128 * 1024);
    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();
    runTask(task, executor,
            makeLambdaTask([&]() { promise.set_value(true);  }));
    (void) future.get();
}

void
TaskRunner::pollTask(Context::SP context)
{
    // run by context executor
    if (context->done()) {
        return;
    }
    if (context->rootTask()->getState() == State::DONE) {
        context->setDone();
        return;
    }
    TaskList readyTasks;
    TaskSet checked;
    getReadyTasks(context->rootTask(), readyTasks, checked);
    internalRunTasks(readyTasks, context);
}

void
TaskRunner::runTask(InitializerTask::SP rootTask,
                    vespalib::Executor &contextExecutor,
                    vespalib::Executor::Task::UP doneTask)
{
    Context::SP context(std::make_shared<Context>(rootTask, contextExecutor,
                                                  std::move(doneTask)));
    context->execute(makeLambdaTask([=]() { pollTask(context); } ));
}

}
