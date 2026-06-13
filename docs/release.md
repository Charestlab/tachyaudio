# Release checklist

This project publishes platform wheels for supported CPython versions instead
of `abi3` wheels.

## Beta release flow

1. Merge the release-prep pull request.
2. Tag the merged commit:

   ```bash
   git checkout main
   git pull --ff-only
   git tag -a v0.2.0b1 -m "v0.2.0b1"
   git push origin v0.2.0b1
   ```

3. Wait for the GitHub Actions `wheels` workflow to finish for the tag.
4. Download the `linux-wheels` and `macos-wheels` artifacts.
5. Build the source distribution from the tagged commit:

   ```bash
   python -m build --sdist
   ```

6. Validate package metadata:

   ```bash
   python -m twine check dist/*
   ```

7. Upload to TestPyPI first:

   ```bash
   python -m twine upload --repository testpypi dist/*
   ```

8. Install from TestPyPI in a clean environment and smoke-test import plus a
   short playback/capture check where hardware is available.
9. Upload to PyPI:

   ```bash
   python -m twine upload dist/*
   ```

10. Create the GitHub release for the tag and attach wheel artifacts.

## Windows support

Windows wheels are intentionally not published until WASAPI support has been
implemented and validated on Windows hardware.
