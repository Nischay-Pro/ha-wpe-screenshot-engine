<!-- Thanks for contributing to the WPE Screenshot Engine. -->

## Summary

<!-- What does this change do, and which issue does it address? -->

## Alternatives considered

<!-- Note relevant approaches considered and why this change was chosen. -->

## Validation

- [ ] `pre-commit run --all-files`
- [ ] `python3 -m unittest discover -s tests -v`
- [ ] `sh tests/run_mcp_fixture_integration.sh` (when Docker/network are available)
- [ ] CI build/test results reviewed for `amd64` and `aarch64`

## Security and privacy

- [ ] No Home Assistant token, private URL, dashboard data, or sensitive screenshot is included.
- [ ] Input validation, logs, and access-token handling were considered where relevant.
