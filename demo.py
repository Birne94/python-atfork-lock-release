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
