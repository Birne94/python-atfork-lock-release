import os
import unittest
import atfork_lock_release


class ForkingTestCase(unittest.TestCase):

    def setUp(self):
        atfork_lock_release.register()

    def tearDown(self):
        atfork_lock_release.deregister()

    def _wait_for_child(self, pid):
        _, result = os.waitpid(pid, 0)
        status, signal = (result & 0xff00) >> 8, result & 0xff
        self.assertEqual(status, 0, "Child process returned non-zero exit "
                                    "code %d (signal %d)" % (status, signal))

    # ---

    def test_forking_handlers_are_called(self):
        pre = post_p = post_c = False

        @atfork_lock_release.pre_fork
        def cb1():
            nonlocal pre
            pre = True

        @atfork_lock_release.after_fork_parent
        def cb2():
            nonlocal post_p
            post_p = True

        @atfork_lock_release.after_fork_child
        def cb3():
            nonlocal post_c
            post_c = True

        pid = os.fork()
        if pid:
            self.assertTrue(pre, "pre-fork hook not called for parent")
            self.assertTrue(post_p, "post-fork hook for parent not called")
            self._wait_for_child(pid)
        else:
            self.assertTrue(pre, "pre-fork hook not called for child")
            self.assertTrue(post_c, "post-fork hook for child not called")

    # ---

    def test_forking_handlers_are_called_in_correct_process(self):
        parent_pid = os.getpid()

        @atfork_lock_release.pre_fork
        def pre():
            self.assertEqual(os.getpid(), parent_pid)

        @atfork_lock_release.after_fork_parent
        def post_p():
            self.assertEqual(os.getpid(), parent_pid)

        @atfork_lock_release.after_fork_child
        def post_c():
            self.assertNotEqual(os.getgid(), parent_pid)

        pid = os.fork()
        if pid:
            self._wait_for_child(pid)

    # ---

    def test_forking_handlers_not_called_if_deactivated(self):
        atfork_lock_release.deregister()

        @atfork_lock_release.pre_fork
        def pre():
            self.fail("pre-fork hook called despite hooks being disabled")

        @atfork_lock_release.after_fork_parent
        def post_p():
            self.fail("post-fork for parent hook called despite "
                      "hooks being disabled")

        @atfork_lock_release.after_fork_child
        def post_c():
            self.fail("post-fork for child hook called despite "
                      "hooks being disabled")

        pid = os.fork()
        if pid:
            self._wait_for_child(pid)
