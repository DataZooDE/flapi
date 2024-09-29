# flAPI: Instant APIs for Static Datasets

flAPI is a powerful service that automatically generates read-only APIs for static datasets without writing a single line of code. Built on top of DuckDB and leveraging its SQL engine and extension ecosystem, flAPI offers a seamless way to connect to various data sources and expose them as RESTful APIs.

## 🚀 Features

- **Automatic API Generation**: Create APIs for your datasets without coding
- **Multiple Data Sources**: Connect to BigQuery, SAP (via ERPL), Parquet, Iceberg, Postgres, MySQL, and more
- **SQL Templates**: Use Jinja2-like syntax for dynamic queries
- **Caching**: Improve performance with built-in caching mechanisms
- **Security**: Implement row-level and column-level security with ease
- **Monitoring**: Built-in access logging and monitoring capabilities

## 🛠 Quick Start

1. Clone the repository:
   ```
   git clone https://github.com/datazoode/flapi.git
   ```

2. Build the project:
   ```
   cd flapi
   mkdir build && cd build
   cmake ..
   make
   ```

3. Run flAPI:
   ```
   ./flapi --config ../examples/flapi.yaml
   ```

## 📖 Example

Here's a simple example of how to create an API endpoint using flAPI:

1. Define your SQL template (`users.sql`):
   ```sql
   SELECT * FROM public.users
   WHERE age >= {{ context.params.age }};
   ```

2. Configure your endpoint (`users.yaml`):
   ```yaml
   urlPath: /users
   request:
     - fieldName: age
       fieldIn: query
   ```

3. Send a request:
   ```
   curl -X GET "http://localhost:8080/api/users?age=18"
   ```

## 🔒 Security

flAPI supports various security features:

- **Row-Level Security**: Control access to individual rows based on user attributes
- **Column-Level Security**: Restrict access to specific columns for certain users
- **Dynamic Data Masking**: Mask sensitive data in query results

## 📚 Documentation

For more detailed information, check out our [full documentation](link-to-your-docs).

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for more details.

## 📄 License

flAPI is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for more details.

## 🙋‍♀️ Support

If you have any questions or need help, please [open an issue](https://github.com/yourusername/flapi/issues) or join our [community chat](link-to-your-chat).

---
