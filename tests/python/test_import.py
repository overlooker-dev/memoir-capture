import memoir_capture


def test_ping():
    assert memoir_capture.ping() == "memoir-capture 0.3.2 loaded OK"


def test_version():
    assert memoir_capture.__version__ == "0.3.2"
