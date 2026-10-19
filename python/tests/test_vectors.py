from simavr_py.vectors import header_name, vector_label, vector_names


def test_header_name():
    assert header_name("attiny3217") == "iotn3217.h"
    assert header_name("atmega4809") == "iom4809.h"
    assert header_name("") is None and header_name("at90can128") is None


def test_names_from_header():
    n = vector_names("attiny3217")
    assert n[0] == "RESET" and n[1] == "CRCSCAN_NMI"
    assert "TCA0_OVF" in n.values()
    assert "USART0_RXC" in vector_names("atmega4809").values()


def test_unknown_device_is_empty():
    assert vector_names("nosuchchip") == {}
    assert vector_names("") == {}


def test_label():
    assert vector_label({3: "FOO"}, 3) == "3 FOO"
    assert vector_label({3: "FOO"}, 9) == "9"
