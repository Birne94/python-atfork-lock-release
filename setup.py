import os
from setuptools import setup, Extension

module = Extension('atfork_lock_release',
                   sources=[
                       'src/atfork_lock_release.c'
                   ])

with open(os.path.join(os.path.dirname(__file__),
                       'README.md'), 'r') as fs:
    long_description = fs.read()


setup(
    name='atfork_lock_release',
    version='0.1',
    description='Module to provide access to posix pthread_atfork function '
                'and resolve I/O related deadlocks while forking.',
    long_description=long_description,
    ext_modules=[module],
    author='Daniel Birnstiel',
    author_email='daniel@birne.me'
)
