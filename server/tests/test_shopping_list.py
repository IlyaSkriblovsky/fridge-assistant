from unittest.mock import Mock

import pytest
from gkeepapi.exception import APIException, KeepException
from gkeepapi.node import List, NewListItemPlacementValue, Note

import shopping_list as shopping


@pytest.mark.parametrize("text", ["МОЛОКО", "  Молоко \t\n", "молоко"])
def test_normalize(text):
    assert shopping.normalize(text) == "молоко"


def test_put_add_restore_and_deduplicate():
    note = List()
    assert shopping.put(note, "  Молоко   2 л ") == shopping.ADDED
    assert note.unchecked[0].text == "Молоко 2 л"
    assert shopping.put(note, "молоко 2 Л") == shopping.ALREADY_LISTED
    item = note.unchecked[0]
    item.checked = True
    assert shopping.put(note, "МОЛОКО 2 л") == shopping.RESTORED
    assert note.unchecked == [item]
    assert shopping.put(note, "молоко") == shopping.ADDED
    assert len(note.items) == 2


def test_unchecked_match_takes_precedence():
    note = List()
    note.add("Молоко", checked=True)
    note.add("молоко")
    assert shopping.put(note, "МОЛОКО") == shopping.ALREADY_LISTED
    assert len(note.checked) == 1


@pytest.mark.parametrize(
    "texts,insertion",
    [
        ([], 0),
        (["Хлеб", "Яблоки"], 2),
        (["Хлеб", "", "Яблоки"], 3),
        (["Хлеб", "", "Яблоки", "", " \t\n\u00a0\u2003"], 3),
        (["", " \t\n\u00a0\u2003"], 0),
    ],
)
@pytest.mark.parametrize("tied_sort_values", [False, True])
def test_add_preserves_placeholders_and_request_order(
    keep, texts, insertion, tied_sort_values
):
    note = keep.get.return_value
    originals = [
        note.add(text, sort=NewListItemPlacementValue.Bottom) for text in texts
    ]
    if tied_sort_values:
        for item in originals:
            item.sort = 100
    checked = note.add("Куплено", checked=True, sort=NewListItemPlacementValue.Bottom)

    result = shopping.add(["молоко", "сыр", "МОЛОКО"])

    assert [entry["result"] for entry in result["items"]] == [
        shopping.ADDED,
        shopping.ADDED,
        shopping.ALREADY_LISTED,
    ]
    assert [item.text for item in note.unchecked] == (
        texts[:insertion] + ["Молоко", "Сыр"] + texts[insertion:]
    )
    assert [item for item in note.unchecked if item in originals] == originals
    assert note.checked == [checked]


def test_restore_keeps_original_position_before_placeholders(keep):
    note = keep.get.return_value
    restored = note.add("молоко", checked=True, sort=NewListItemPlacementValue.Bottom)
    bread = note.add("Хлеб", sort=NewListItemPlacementValue.Bottom)
    blank = note.add("", sort=NewListItemPlacementValue.Bottom)

    assert shopping.add(["молоко"])["items"][0]["result"] == shopping.RESTORED
    assert note.unchecked == [restored, bread, blank]


@pytest.mark.parametrize("blank_tail", [[], ["", " \t\u00a0"]])
def test_indented_placeholder_stays_with_parent(keep, blank_tail):
    note = keep.get.return_value
    bread = note.add("Хлеб", sort=NewListItemPlacementValue.Bottom)
    child = bread.add("", sort=NewListItemPlacementValue.Bottom)
    placeholders = [
        note.add(text, sort=NewListItemPlacementValue.Bottom) for text in blank_tail
    ]
    original_sort = (bread.sort, child.sort)

    shopping.add(["молоко", "сыр"])

    assert [item.text for item in note.unchecked] == [
        "Хлеб",
        "",
        "Молоко",
        "Сыр",
        *blank_tail,
    ]
    assert child.parent_item is bread
    assert bread.subitems == [child]
    assert all(not item.indented for item in note.unchecked if item is not child)
    assert all(item in note.unchecked for item in placeholders)
    if not blank_tail:
        assert (bread.sort, child.sort) == original_sort


@pytest.fixture
def keep(monkeypatch):
    client = Mock()
    client.get.return_value = List()
    monkeypatch.setattr(shopping.gkeepapi, "Keep", Mock(return_value=client))
    return client


def test_add_syncs_and_handles_repeated_items(keep):
    assert shopping.add(["Молоко", " \t", "молоко", "Хлеб"]) == {
        "items": [
            {"item": "Молоко", "result": "added"},
            {"item": "молоко", "result": "already_listed"},
            {"item": "Хлеб", "result": "added"},
        ]
    }
    keep.authenticate.assert_called_once_with("test", "test", sync=False)
    keep.get.assert_called_once_with("test")
    assert keep.sync.call_count == 2
    assert [call[0] for call in keep.mock_calls] == [
        "authenticate",
        "sync",
        "get",
        "sync",
    ]


@pytest.mark.parametrize("items", [[], ["", " \n\t"]])
def test_empty_input_does_not_create_client(items):
    assert shopping.add(items) == {"error": "no items given"}


@pytest.mark.parametrize("kind", ["missing", "note", "trashed"])
def test_invalid_note(keep, kind):
    note = {"missing": None, "note": Note(), "trashed": List()}[kind]
    if kind == "trashed":
        note.trash()
    keep.get.return_value = note
    assert shopping.add(["Хлеб"]) == {
        "error": "the shopping list is missing from Google Keep"
    }
    assert keep.sync.call_count == 1


@pytest.mark.parametrize(
    "error", [KeepException("broken"), APIException(500, "broken"), OSError("offline")]
)
@pytest.mark.parametrize("stage", ["authenticate", "initial_sync", "final_sync"])
def test_service_errors(keep, error, stage):
    if stage == "authenticate":
        keep.authenticate.side_effect = error
    else:
        keep.sync.side_effect = [error] if stage == "initial_sync" else [None, error]
    assert shopping.add(["Хлеб"]) == {"error": f"Google Keep failed: {error}"}


@pytest.mark.parametrize(
    "text,expected",
    [
        ("  молоко   UHT ", "Молоко UHT"),
        ("iPhone", "IPhone"),
        ("2 л молока", "2 Л молока"),
        ("«сыр»", "«Сыр»"),
    ],
)
def test_added_items_start_with_uppercase(keep, text, expected):
    shopping.add([text])
    assert keep.get.return_value.unchecked[0].text == expected


@pytest.mark.parametrize("checked", [False, True])
def test_existing_items_are_capitalized_without_changing_other_letters(keep, checked):
    note = keep.get.return_value
    note.add("молоко UHT", checked=checked)
    shopping.add(["молоко UHT"])
    assert len(note.items) == 1
    assert note.unchecked[0].text == "Молоко UHT"
