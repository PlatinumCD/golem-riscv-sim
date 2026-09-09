"""Required-counter compatibility contract exercised by the host regression."""

class RequiredMetrics(dict):
    """Legacy analyzers may supply a fallback; absent evidence still fails closed."""
    def get(self, key, default=None):
        if key not in self or self[key] is None:
            raise ValueError(f"required measured counter missing: {key}")
        return self[key]
