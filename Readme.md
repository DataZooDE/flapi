# flAPI: Instant APIs for Static Datasets

## What is flAPI?

flAPI is a powerful service that automatically generates read-only APIs for static datasets without writing a single line of code. It offers a seamless way to expose various data sources as RESTful APIs, making it easy to integrate and access your data.

## How is flAPI related to DuckDB?

flAPI is built on top of DuckDB, leveraging its efficient SQL engine and extensive extension ecosystem. This integration allows flAPI to connect to a wide range of data sources and perform fast, in-memory operations on your datasets.

## What Painpoints does flAPI solve and what can you achieve?

flAPI addresses several common challenges in data access and API creation:

1. **Rapid API Development**: Create APIs for your datasets instantly, without writing backend code.
2. **Data Source Flexibility**: Connect to various data sources like BigQuery, SAP (via ERPL), Parquet, Iceberg, Postgres, MySQL, and more.
3. **Query Customization**: Use Jinja2-like syntax for dynamic SQL queries.
4. **Performance Optimization**: Improve response times with built-in caching mechanisms.
5. **Security Implementation**: Easily implement row-level and column-level security.
6. **Monitoring and Logging**: Built-in access logging and monitoring capabilities.

With flAPI, you can:
- Quickly expose datasets as APIs for internal or external use
- Create data-driven applications without complex backend setups
- Implement secure data access controls
- Optimize query performance for large datasets

## How to build locally?

To build flAPI from source:

1. Clone the repository:
   ```
   git clone https://github.com/datazoode/flapi.git
   ```

2. Navigate to the project directory:
   ```
   cd flapi
   ```

3. Build the project:
   ```
   make release
   ```

   This will create a release build in the `build/Release` directory.

## How to run flAPI?

1. Download the latest release from our [releases page](https://github.com/datazoode/flapi/releases) or build from source.
2. Create a configuration file (e.g., `flapi.yaml`) with your desired settings.
3. Run flAPI:
   ```
   ./flapi --config flapi.yaml --log-level info
   ```

   You can adjust the log level (debug, info, warning, error) as needed.

## Basic Configuration of flAPI

Here's a simple example of a flAPI configuration file:

```yaml
server:
  port: 8080

datasources:
  - name: my_database
    type: postgres
    connection_string: "postgresql://user:password@localhost:5432/mydb"

endpoints:
  - name: users
    datasource: my_database
    query: "SELECT * FROM users WHERE age >= {{ context.params.age }}"
    url_path: /users
    request:
      - fieldName: age
        fieldIn: query
```

This configuration sets up a simple API endpoint that queries a Postgres database and filters users by age.

## How to build the docker container?

To build and run flAPI using Docker:

1. Build the Docker image:
   ```
   docker build --build-arg BUILDKIT_INLINE_CACHE=1 -t flapi:latest .
   ```

2. Run the container:
   ```
   docker run -p 8080:8080 -v /path/to/your/config:/app/config flapi
   ```

   Replace `/path/to/your/config` with the actual path to your configuration directory.

## 📚 Documentation

For more detailed information, including advanced features and API reference, check out our [full documentation](link-to-your-docs).

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for more details.

## 📄 License

flAPI is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for more details.

## 🙋‍♀️ Support

If you have any questions or need help, please [open an issue](https://github.com/datazoode/flapi/issues) or join our [community chat](link-to-your-chat).

---
