from setuptools import setup, Extension

module_sources = [
    'src/atfork_lock_release.c'
]

module = Extension('atfork_lock_release',
                   sources=module_sources)

setup(
    name='atfork_lock_release',
    version='0.1',
    description='Module to provide access to posix pthread_atfork function',
    ext_modules=[module],
    author='Daniel Birnstiel',
    author_email='daniel@birne.me'
)
