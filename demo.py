import atfork_lock_release
import os


def main():
    pid = os.fork()
    print("fork:", pid)


def pre_fork():
    print("pre-fork", os.getpid())


def after_fork_parent():
    print("parent", os.getpid())


def after_fork_child():
    print("child", os.getpid())


if __name__ == '__main__':
    atfork_lock_release.register()
    atfork_lock_release.pre_fork(pre_fork)
    atfork_lock_release.after_fork_parent(after_fork_parent)
    atfork_lock_release.after_fork_child(after_fork_child)

    main()
