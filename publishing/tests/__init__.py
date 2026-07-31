# Marks publishing/tests as a package so `unittest discover` can name the test
# modules relative to publishing/, which is what puts publishing/ on sys.path
# and makes `import publish_newsletter` work from inside them.
