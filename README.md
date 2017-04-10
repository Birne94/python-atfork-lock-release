# python-atfork-lock-release

Adds support for freeing io locks after fork in order to avoid deadlocks. Requires pthread support in order to work as
it relies on [`pthread_atfork(3)`](https://linux.die.net/man/3/pthread_atfork).

If a process is forked while an io lock is being held, the child process will deadlock on the next call to `flush`.
This is prevented by checking for acquired locks after fork and manually freeing them. This may cause corruption in the
the file stream as concurrent writes betweeen the two processes are no longer protected.

Note that this has only been tested using Python 3.6 on OSX so far, but it should work on other systems as well 
(Windows might cause problems, though).

## Installation

Install from source using

```sh
python setup.py install
```

## Usage

Enable the atfork hooks through `register` 

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