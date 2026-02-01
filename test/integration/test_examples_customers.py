"""
Integration tests for examples/sqls/customers/ endpoints.
These endpoints require basic authentication (admin:secret).

Note: Due to server stability issues with DuckLake cache operations,
these tests are limited to avoid triggering the cache refresh hang bug.

Reuses:
- examples_server fixture (from conftest.py)
- examples_url fixture (from conftest.py)
- examples_auth fixture (from conftest.py)
- wait_for_examples fixture (from conftest.py)
"""
import pytest
import requests


class TestExamplesCustomersREST:
    """Test customer REST endpoints with authentication.

    Note: Tests are limited to avoid triggering server hang issues
    related to DuckLake cache operations after ~5 requests.
    """

    @pytest.fixture(scope="class", autouse=True)
    def setup_class(self, examples_server, wait_for_examples):
        """Ensure examples server is ready before this test class.

        Uses class scope so we only wait once for the server, not before each test.
        This avoids timing issues with the server's background tasks.
        """
        pass

    def test_customers_endpoint_requires_auth(self, examples_url):
        """Verify endpoint returns 401 without authentication."""
        response = requests.get(f"{examples_url}/customers/", timeout=10)

        assert response.status_code == 401

    def test_customers_endpoint_returns_data(self, examples_url, examples_auth):
        """Verify basic endpoint functionality with auth."""
        response = requests.get(
            f"{examples_url}/customers/",
            auth=examples_auth,
            timeout=10
        )

        assert response.status_code == 200
        data = response.json()
        assert "data" in data
        assert len(data["data"]) > 0

    def test_segment_filter_automobile(self, examples_url, examples_auth):
        """Verify segment enum validator works with AUTOMOBILE."""
        response = requests.get(
            f"{examples_url}/customers/",
            params={"segment": "AUTOMOBILE"},
            auth=examples_auth,
            timeout=10
        )

        assert response.status_code == 200
        data = response.json()
        assert "data" in data
        # All returned customers should have AUTOMOBILE segment
        for customer in data["data"]:
            # The segment field is nested in the customer data
            assert "c_mktsegment" in customer or "segment" in customer
            segment_value = customer.get("c_mktsegment") or customer.get("segment")
            if isinstance(segment_value, dict):
                segment_value = segment_value.get("segment") or segment_value.get("c_mktsegment")
            assert segment_value == "AUTOMOBILE"

    def test_wrong_password_rejected(self, examples_url):
        """Verify wrong password is rejected."""
        response = requests.get(
            f"{examples_url}/customers/",
            auth=("admin", "wrong_password"),
            timeout=10
        )

        assert response.status_code == 401

    def test_wrong_username_rejected(self, examples_url):
        """Verify wrong username is rejected."""
        response = requests.get(
            f"{examples_url}/customers/",
            auth=("wrong_user", "secret"),
            timeout=10
        )

        assert response.status_code == 401
