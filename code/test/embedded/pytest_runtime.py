import pytest
from pytest_embedded_idf import IdfDut


@pytest.mark.embedded
def test_runtime_unity(dut: IdfDut) -> None:
    cases = dut.test_menu
    assert len(cases) == 10, "Unity test registration is incomplete"
    assert {group for case in cases for group in case.groups} == {"ble", "log", "ui", "fit"}
    dut.run_all_single_board_cases(timeout=30)
    assert dut.testsuite.attrs.get("failures", 0) == 0