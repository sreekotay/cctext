"""Module docstring: cctext embed fixture.

Contains the word def and a fence-like ``` sequence on purpose.
"""
import re

SQL = """
SELECT id, name
FROM users
WHERE active = 1
  AND name LIKE '%o''brien%'
"""


def greet(name):
    """Return a greeting for ``name``.

    >>> greet("x")
    'hi x'
    """
    return f"hi {name!r} {{literal braces}} {len(name) + 1}"


def find(text):
    '''Single-quoted docstring with "double" quotes inside.'''
    pat = r"^\d+\.\s+(\w+)\s*$"
    return re.match(pat, text)


ESCAPED = "a \"\"\" b"  # body contains an escaped triple quote
NESTED = "she said 'it''s \"fine\"'"
# def not_a_def(): """ this is a comment, not a docstring """
MIXED = 'single with "double" inside' + "double with 'single' inside"
