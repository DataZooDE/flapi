# Northwind Database Examples

Complete flapi read/write examples for the Northwind SQLite database.

## Database Connection

The database is configured in `examples/flapi.yaml` as `northwind-sqlite` connection:
- Database file: `./examples/data/northwind.sqlite`
- Attached schema: `nw`

## Endpoints

### Customers (Read-Only)

**GET** `/northwind/customers/`
- List customers with optional filtering by:
  - `customer_id` (string, e.g., "ALFKI")
  - `company_name` (partial match)
  - `country`
  - `city`
- Supports pagination
- Files: `customers.sql`, `customers.yaml`

### Products (Full CRUD)

**GET** `/northwind/products/`
- List products with optional filtering by:
  - `product_name` (partial match)
  - `category_id`
  - `supplier_id`
  - `discontinued` (0 or 1)
- Supports pagination
- Files: `products.sql`, `products-list.yaml`

**GET** `/northwind/products/<product_id>`
- Get single product by ID
- Files: `products.sql`, `products-get.yaml`

**POST** `/northwind/products/`
- Create new product
- Required fields: `product_name`, `supplier_id`, `category_id`
- Optional fields: `quantity_per_unit`, `unit_price`, `units_in_stock`, `units_on_order`, `reorder_level`, `discontinued`
- Returns created product with `RETURNING` clause
- Files: `products-create.sql`, `products-create.yaml`

**PUT** `/northwind/products/<product_id>`
- Update existing product
- All fields optional (partial update)
- Returns updated product with `RETURNING` clause
- Files: `products-update.sql`, `products-update.yaml`

**DELETE** `/northwind/products/<product_id>`
- Delete product by ID
- Files: `products-delete.sql`, `products-delete.yaml`

### Orders with Order Details (Read-Only)

**GET** `/northwind/orders/`
- List orders with associated order details joined
- Each order includes an array of order details (ProductID, UnitPrice, Quantity, Discount)
- Optional filtering by:
  - `order_id`
  - `customer_id` (string, e.g., "ALFKI")
  - `order_date` (YYYY-MM-DD)
  - `shipped_date` (YYYY-MM-DD)
- Supports pagination
- Files: `orders.sql`, `orders.yaml`

## Usage Examples

### Get Customers
```bash
curl http://localhost:8080/northwind/customers/
curl http://localhost:8080/northwind/customers/?country=USA
curl http://localhost:8080/northwind/customers/?customer_id=ALFKI
```

### Products CRUD
```bash
# List products
curl http://localhost:8080/northwind/products/

# Get product by ID
curl http://localhost:8080/northwind/products/1

# Create product
curl -X POST http://localhost:8080/northwind/products/ \
  -H "Content-Type: application/json" \
  -d '{
    "product_name": "New Product",
    "supplier_id": 1,
    "category_id": 1,
    "unit_price": "19.99",
    "units_in_stock": 100
  }'

# Update product
curl -X PUT http://localhost:8080/northwind/products/1 \
  -H "Content-Type: application/json" \
  -d '{
    "product_name": "Updated Product Name",
    "unit_price": "24.99"
  }'

# Delete product
curl -X DELETE http://localhost:8080/northwind/products/1
```

### Get Orders
```bash
curl http://localhost:8080/northwind/orders/
curl http://localhost:8080/northwind/orders/?customer_id=ALFKI
curl http://localhost:8080/northwind/orders/?order_id=10248
```

## Database Schema

Based on the Northwind database structure:
- **Customers**: CustomerID (PK, string), CompanyName, ContactName, ContactTitle, Address, City, Region, PostalCode, Country, Phone, Fax
- **Products**: ProductID (PK, int), ProductName, SupplierID, CategoryID, QuantityPerUnit, UnitPrice, UnitsInStock, UnitsOnOrder, ReorderLevel, Discontinued
- **Orders**: OrderID (PK, int), CustomerID, EmployeeID, OrderDate, RequiredDate, ShippedDate, ShipVia, Freight, ShipName, ShipAddress, ShipCity, ShipRegion, ShipPostalCode, ShipCountry
- **Order Details**: OrderID (PK/FK), ProductID (PK/FK), UnitPrice, Quantity, Discount

## Notes

- All write operations (POST, PUT, DELETE) use transactions by default
- Products create/update operations return the created/updated record using `RETURNING` clauses
- Orders query uses `LIST()` aggregation to combine Order Details into a JSON array per order
- String fields use triple braces `{{{ }}}` in templates for proper SQL escaping
- Numeric fields use double braces `{{ }}` as they don't need quotes

