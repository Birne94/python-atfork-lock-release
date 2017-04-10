# python-atfork-lock-release

Adds support for freeing io locks after fork in order to avoid deadlocks. Requires pthread support in order to work as
it relies on [`pthread_atfork(3)`](https://linux.die.net/man/3/pthread_atfork).