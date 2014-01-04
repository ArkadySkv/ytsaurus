import pytest

from yt_env_setup import YTEnvSetup
from yt_commands import *

import sys

##################################################################

@pytest.mark.skipif("True")
class TestQuery(YTEnvSetup):
    NUM_MASTERS = 3
    NUM_NODES = 3
    NUM_SCHEDULERS = 0

    def _sample_data(self, path="//tmp/t", chunks=3, stripe=3):
        create("table", path)

        for i in xrange(chunks):
            data = [
                {"a": (i * stripe + j), "b": (i * stripe + j) * 10}
                for j in xrange(1, 1 + stripe)]
            write("<append=true>" + path, data)

        set(path + "/@schema", [
            {"name": "a", "type": "integer"},
            {"name": "b", "type": "integer"}
        ])

    # TODO(sandello): TableMountCache is not invalidated at the moment,
    # so table names must be unique.
    def test_simple(self):
        for i in xrange(0, 50, 10):
            path = "//tmp/t{0}".format(i)

            self._sample_data(path=path, chunks=i, stripe=10)
            result = select("a, b from [{0}]".format(path), verbose=False)

            assert len(result) == 10 * i

    def test_project1(self):
        self._sample_data(path="//tmp/p1")
        expected = [{"s": 2 * i + 10 * i - 1} for i in xrange(1, 10)]
        actual = select("2 * a + b - 1 as s from [//tmp/p1]")
        assert expected == actual

    def test_group_by1(self):
        self._sample_data(path="//tmp/g1")
        expected = [{"s": 450}]
        actual = select("sum(b) as s from [//tmp/g1] group by 1 as k")
        self.assertItemsEqual(expected, actual)

    def test_group_by2(self):
        self._sample_data(path="//tmp/g2")
        expected = [{"k": 0, "s": 200}, {"k": 1, "s": 250}]
        actual = select("k, sum(b) as s from [//tmp/g2] group by a % 2 as k")
        self.assertItemsEqual(expected, actual)
