"""Russian text formatting helpers."""


def plural_form(count: int, one: str, few: str, many: str) -> str:
    """Choose the form for an integer; supply forms used with 1, 2, and 5."""
    count = abs(count)
    if 11 <= count % 100 <= 14:
        return many
    if count % 10 == 1:
        return one
    if 2 <= count % 10 <= 4:
        return few
    return many
