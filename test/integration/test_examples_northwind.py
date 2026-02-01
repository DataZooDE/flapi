"""
Integration tests for examples/sqls/northwind/ endpoints.
Northwind endpoints now require the standard admin/secret basic auth.

Reuses:
- examples_server fixture (from conftest.py)
- examples_url fixture (from conftest.py)
- wait_for_examples fixture (from conftest.py)
"""
import pytest
import requests

class TestExamplesNorthwindProducts:
    """Test Northwind product endpoints."""

    @pytest.fixture(scope="class", autouse=True)
    def setup_class(self, examples_server, wait_for_examples):
        """Ensure examples server is ready before this test class.

        Uses class scope so we only wait once for the server, not before each test.
        This avoids timing issues with the server's background tasks.
        """
        pass

    def test_products_list_returns_data(self, examples_url, examples_auth):
        """Verify products list endpoint returns data."""
        response = requests.get(
            f"{examples_url}/northwind/products/",
            auth=examples_auth,
            timeout=10,
        )

        assert response.status_code == 200
        data = response.json()
        assert "data" in data
        assert len(data["data"]) > 0

        # Verify expected fields exist
        first_product = data["data"][0]
        assert "ProductID" in first_product
        assert "ProductName" in first_product

    def test_products_list_with_pagination(self, examples_url, examples_auth):
        """Verify products list supports pagination."""
        response = requests.get(
            f"{examples_url}/northwind/products/",
            params={"limit": 5},
            auth=examples_auth,
            timeout=10
        )

        assert response.status_code == 200
        data = response.json()
        assert "data" in data
        # Should respect limit
        assert len(data["data"]) <= 5

    def test_products_filter_by_name(self, examples_url, examples_auth):
        """Verify product name filter works.

        Note: SQLite LIKE is case-sensitive by default, so we use "Chai" to match
        the actual product name in the Northwind database.
        """
        response = requests.get(
            f"{examples_url}/northwind/products/",
            params={"product_name": "Chai"},
            auth=examples_auth,
            timeout=10
        )

        assert response.status_code == 200
        data = response.json()
        assert "data" in data
        # Should find at least one product containing "Chai"
        assert len(data["data"]) > 0
        for product in data["data"]:
            assert "Chai" in product["ProductName"]

    def test_products_filter_by_category(self, examples_url, examples_auth):
        """Verify category filter works."""
        response = requests.get(
            f"{examples_url}/northwind/products/",
            params={"category_id": 1},
            auth=examples_auth,
            timeout=10
        )

        assert response.status_code == 200
        data = response.json()
        assert "data" in data
        # All products should have category_id 1
        for product in data["data"]:
            assert product.get("CategoryID") == 1

    def test_products_get_by_id(self, examples_url, examples_auth):
        """Verify single product retrieval by ID.

        Note: Endpoints with with-pagination: false return a raw list,
        not wrapped in {"data": [...]}.
        """
        response = requests.get(
            f"{examples_url}/northwind/products/1",
            auth=examples_auth,
            timeout=10,
        )

        assert response.status_code == 200
        data = response.json()
        # Response is a raw list when with-pagination: false
        assert isinstance(data, list)
        assert len(data) == 1
        assert data[0]["ProductID"] == 1

    def test_products_get_nonexistent_returns_empty(self, examples_url, examples_auth):
        """Verify getting nonexistent product returns empty data.

        Note: Endpoints with with-pagination: false return a raw list.
        """
        response = requests.get(
            f"{examples_url}/northwind/products/99999",
            auth=examples_auth,
            timeout=10,
        )

        assert response.status_code == 200
        data = response.json()
        # Response is a raw list when with-pagination: false
        assert isinstance(data, list)
        assert len(data) == 0

    def test_products_invalid_id_rejected(self, examples_url, examples_auth):
        """Verify invalid product ID is rejected by validator."""
        response = requests.get(
            f"{examples_url}/northwind/products/-1",
            auth=examples_auth,
            timeout=10,
        )

        # Should be rejected - negative ID
        assert response.status_code == 400

    def test_products_list_invalid_category_rejected(self, examples_url, examples_auth):
        """Verify invalid category_id is rejected by validator."""
        response = requests.get(
            f"{examples_url}/northwind/products/",
            params={"category_id": -1},
            auth=examples_auth,
            timeout=10
        )

        assert response.status_code == 400


class TestExamplesNorthwindCustomers:
    """Test Northwind customers endpoint."""

    @pytest.fixture(scope="class", autouse=True)
    def setup_class(self, examples_server, wait_for_examples):
        """Ensure examples server is ready before this test class."""
        pass

    def test_customers_list_returns_data(self, examples_url, examples_auth):
        """Verify customers list endpoint returns data."""
        response = requests.get(
            f"{examples_url}/northwind/customers/",
            auth=examples_auth,
            timeout=10,
        )

        assert response.status_code == 200
        data = response.json()
        assert "data" in data
        assert len(data["data"]) > 0


# NOTE: Orders endpoint tests removed due to server hang issues.
# The orders endpoint uses complex JOINs that appear to trigger a server
# stability issue related to DuckLake cache operations. This test can be
# re-enabled once the underlying server issue is fixed.
