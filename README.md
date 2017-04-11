# python-atfork-lock-release

Adds support for freeing io locks after fork in order to avoid deadlocks. Requires pthread support in order to work as
it relies on [`pthread_atfork(3)`](https://linux.die.net/man/3/pthread_atfork).

If a process is forked while an io lock is being held, the child process will deadlock on the next call to `flush`.
This is prevented by checking for acquired locks after fork and manually freeing them. This may cause corruption in the
the file stream as concurrent writes betweeen the two processes are no longer protected.

Note that this has only been tested using Python 3.6 on OSX and 3.4 on Ubuntu so far, but it should work on other 
systems as well (Windows might cause problems, though).

## Installation

Install from source using

```sh
python setup.py install
```

## Preventing I/O deadlocks

The module provides hooks in order to prevent deadlocks while forking. The hooks can be enabled for `stdout`/`stderr` 
through `register()`. 

```python
import atfork_lock_release
atfork_lock_release.register()
```

Behavior can be returned back to normal using

```python
atfork_lock_release.deregister()
```

While this keeps the atfork handlers in place (there is no removing counterpart to `pthread_atfork`), they are disabled
until `register` is called again.

File handles might cause deadlocks as well, they can be monitored through `watch()`:

```python
fs = open('foo.bar', 'w')
atfork_lock_release.watch(fs)
```

Note that depending on the platform and usage the file contents might get corrupted by concurrent writes, so only use 
with caution. Currently, only text-mode files are supported (opened _without_ the `b` flag).

## Custom atfork-hooks

The module provides an api for adding custom pre- and post-fork hooks:

```python
import atfork_lock_release
import os


def main():
    pid = os.fork()
    print("fork:", pid)


@atfork_lock_release.pre_fork
def pre_fork():
    print("pre-fork", os.getpid())


@atfork_lock_release.after_fork_parent
def after_fork_parent():
    print("parent", os.getpid())


@atfork_lock_release.after_fork_child
def after_fork_child():
    print("child", os.getpid())


if __name__ == '__main__':
    atfork_lock_release.register()
    main()
```

Multiple handlers for each hooks can be registered, the most recently registered one will be called first.