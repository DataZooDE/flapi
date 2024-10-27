# flAPI: Instant SQL based APIs

flAPI is a powerful service that automatically generates read-only APIs for datasets by utilizing SQL templates. Built on top of [DuckDB](https://duckdb.org/) and leveraging its SQL engine and extension ecosystem, flAPI offers a seamless way to connect to various data sources and expose them as RESTful APIs.

![overview of flAPI](https://i.imgur.com/m7UVZlR.png)

## ⚡ Features

- **Automatic API Generation**: Create APIs for your datasets without coding
- **Multiple Data Sources**: Connect to [BigQuery](https://github.com/hafenkran/duckdb-bigquery), SAP ERP & BW (via [ERPL](https://github.com/datazoode/erpl)), Parquet, [Iceberg](https://github.com/duckdb/duckdb_iceberg), [Postgres](https://github.com/duckdb/postgres_scanner), [MySQL](https://github.com/duckdb/duckdb_mysql), and more
- **SQL Templates**: Use Mustache-like syntax for dynamic queries
- **Caching**: Improve performance and reduce database costs with built-in caching mechanisms
- **Security**: Implement row-level and column-level security with ease
- **Easy deployment**: Deploy flAPI with a single binary file

## 🛠 Quick Start
The easiest way to get started with flAPI is to use the pre-built binary for your operating system.

#### 1. Download the binary for your operating system from the [Releases](https://github.com/datazoode/flapi/releases) page.

We currently support the following operating systems:

- Linux

and have that statically linked against [DuckDB v1.1.2](https://github.com/duckdb/duckdb/releases/tag/v1.1.2).

#### 2. Run flAPI:
Once you have downloaded the binary, you can run flAPI by executing the following command:

```
> ./flapi --config ../examples/flapi.yaml
```

#### 3. Test the API server:
If everything is set up correctly, you should be able to access the API at the URL specified in the configuration file.

```bash
> curl 'http://localhost:8080/'


         ___
     ___( o)>   Welcome to
     \ <_. )    flAPI
      `---'    

    Fast and Flexible API Framework
    powered by DuckDB
```

## 🎓 Example

Here's a simple example of how to create an API endpoint using flAPI:

### 1. Create a basic flAPI configuration
flAPI uses the popular [YAML](https://yaml.org/) format to configure the API endpoints. A basic configuration file looks like this:

```yaml
project_name: example-flapi-project
project_description: An example flAPI project demonstrating various configuration options
template:
  path: './sqls'            # The path where SQL templates and API endpoint configurations are stored
  environment-whitelist:    # Optional: List of regular expressions for whitelisting envvars which are available in the templates
    - '^FLAPI_.*'

duckdb:                     # Configuration of the DuckDB embedded into flAPI
  db_path: ./flapi_cache.db # Optional: remove or comment out for in-memory database, we use this store also as cache
  access_mode: READ_WRITE   # See the https://duckdb.org/docs/configuration/overview) for more details
  threads: 8
  max_memory: 8GB
  default_order: DESC

connections:                # A YAML map of database connection configurations, a API endpoint needs to reference one of these connections
   bigquery-lakehouse: 
                            # SQL commands to initialize the connection (e.g., e.g. installing, loading and configuring the BQ a DuckDB extension)
      init: |
         INSTALL 'bigquery' FROM 'http://storage.googleapis.com/hafenkran';
         LOAD 'bigquery';
      properties:           # A YAML map of connection-specific properties (accessible in templates via {{ context.conn.property_name }})
         project_id: 'my-project-id'

   customers-parquet: 
      properties:
         path: './data/customers.parquet'

heartbeat:
  enabled: true            # The eartbeat worker is a background thread which can can be used to periodically trigger endpionts
  worker-interval: 10      # The interval in seconds at which the heartbeat worker will trigger endpoints

enforce-https:
  enabled: false           # Whether to force HTTPS for the API connections, we strongly recommend to use a reverse proxy to do SSL termination
  # ssl-cert-file: './ssl/cert.pem'
  # ssl-key-file: './ssl/key.pem'

```

After that ensure that the template path (`./sqls` in this example) exists.

### 1. Define your API endpoint (`./sqls/customers.yaml`):
Each endpoint is at least defined by a YAML file and a corresponding SQL template in the template path. For our example we will create the file `./sqls/customers.yaml`:

```yaml
url-path: /customers/      # The URL path at which the endpoint will be available

request:                  # The request configuration for the endpoint, this defines the parameters that can be used in the query
  - field-name: id
    field-in: query       # The location of the parameter, other options are 'path', 'query' and 'body'
    description: Customer ID # A description of the parameter, this is used in the auto-generated API documentation
    required: false       # Whether the parameter is required
    validators:           # A list of validators that will be applied to the parameter
      - type: int
        min: 1
        max: 1000000
        preventSqlInjection: true

template-source: customers.sql # The path to the SQL template that will be used to generate the endpoint
connection: 
  - customers-parquet          # The connection that will be used to execute the query

rate-limit:
  enabled: true           # Whether rate limiting is enabled for the endpoint
  max: 100                # The maximum number of requests per interval
  interval: 60            # The interval in seconds
  
auth:
  enabled: true           # Whether authentication is enabled for the endpoint
  type: basic             # The type of authentication, other options are 'basic' and 'bearer'
  users:                  # The users that are allowed to access the endpoint
    - username: admin
      password: secret
      roles: [admin]
    - username: user
      password: password
      roles: [read]

heartbeat:
  enabled: true           # Whether the heartbeat worker if enabled will trigger the endpoint periodically
  params:                 # A YAML map of parameters that will be passed by the heartbeat worker to the endpoint
    id: 123

```
There are many more configuration options available, see the [full documentation](link-to-your-docs) for more details.

### 2. Configure the endpoints SQL template (`./sqls/customers.sql`):
After the creation of the YAML endpoint configuration we need to connect the SQL template which connects the enpoint to the data connection.
The template files use the [Mustache templating language](https://mustache.github.io/) to dynamically generate the SQL query. 

```sql
SELECT * FROM '{{{conn.path}}}'
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{{ params.id }}}
{{/params.id}}
```

The above template uses the `path` parameter defined in the connection configuration to directly query a local parquet file. If the id parameter is 
provided, it will be used to filter the results.

### 3. Send a request:
To test the endpoint and see if everything worked, we can use curl. We should also provide the correct basic auth credentials (`admin:secret` in this case). To make the JSON result easier to read, we pipe the output to [`jq`](https://jqlang.github.io/jq/).

```bash
> curl -X GET -u admin:secret "http://localhost:8080/customers?id=123" | jq .

{
  "next": "",
  "total_count": 1,
  "data": [
    {
      "c_mktsegment": "BUILDING",
      "c_acctbal": 5897.82999999999992724,
      "c_phone": "15-817-151-1168",
      "c_address": "YsOnaaER8MkvK5cpf4VSlq",
      "c_nationkey": 5,
      "c_name": "Customer#000000123",
      "c_comment": "ependencies. regular, ironic requests are fluffily regu",
      "c_custkey": 123
    }
  ]
}

```

## ⁉️ How caching works?
flAPI implements a powerful and flexible caching mechanism to optimize query performance and reduce load on data sources. Here's how it works:

1. Cache Configuration:
   In the endpoint YAML file (e.g., `products.yaml`), you can define caching parameters:
   ```yaml
   cache:
     cache-table-name: 'products_cache'
     cache-source: products_cache.sql
     refresh-time: 15m 
   ```
   - `cache-table-name`: The name of the cache table.
   - `cache-source`: SQL file to create and populate the cache.
   - `refresh-time`: How often the cache should be refreshed.

2. Cache Creation:
   The `cache-source` SQL file (e.g., `products_cache.sql`) defines how to create and populate the cache:
   ```sql
   CREATE TABLE {{cache.schema}}.{{cache.table}} AS
   SELECT 
       p.category,
       p.brand,
       p.price_range,
       p.region,
       sum(p.sales) as total_sales
   FROM bigquery_scan('my-data-warehouse.sales.product_transactions') AS p
   GROUP BY 1, 2, 3, 4
   ```
   This query aggregates data from the source, potentially reducing the data volume and precomputing expensive operations.

3. Cache Usage:
   The main query (e.g., `products.sql`) then uses the cache:
   ```sql
   SELECT 
       p.category,
       p.brand,
       p.price_range,
       p.region,
       p.total_sales
   FROM {{{cache.table}}} AS p
   WHERE 1=1
   {{#params.category}}
    AND p.category LIKE '{{{ params.category }}}'
   {{/params.category}}
   {{#params.region}}
    AND p.region LIKE '{{{ params.region }}}'
   {{/params.region}}
   ```
   This query runs against the cache table instead of the original data source.

4. Cache Management:
   flAPI handles cache creation, refreshing, and cleanup:
   - It checks if caches need refreshing based on the `refresh-time`.
   - Creates new cache tables with timestamps in their names.
   - Keeps a configurable number of previous cache versions.
   - Performs garbage collection to remove old cache tables.
   - When a `HTTP DELETE` request the cache table is also invalidated.

5. Benefits:
   - Improved query performance by querying pre-aggregated data.
   - Reduced load (and thereby costs) on the original data source.
   - Always fresh data with periodic cache refreshes.
   - Ability to fall back to previous cache versions if needed.

This caching mechanism provides a balance between data freshness and query performance, making it ideal for scenarios where real-time data is not critical but fast query responses are important. For example, in our products scenario, it allows for quick analysis of sales data across different categories, brands, price ranges, and regions without repeatedly querying the entire transaction history.



## 🏭 Building from source
The source code of flAPI is written in C++ and closely resembles the [DuckDB build process](https://duckdb.org/docs/dev/building/overview). A good documentation of the build process is the GitHub action in [`build.yaml`](.github/workflows/build.yaml). In essecence a few prerequisites need to be met:
In essecence a few prerequisites need to be met:

- Install the dependencies: `sudo apt-get install -y build-essential cmake ninja-build`
- Checkout the repository and submodules: `git clone --recurse-submodules https://github.com/datazoode/flapi.git`
- Build the project: `make release`

The build process will download and build DuckDB v1.1.2 and install the vcpkg package manager. We depend on the following vcpkg ports:

- [`argparse`](https://github.com/p-ranav/argparse) - Command line argument parser
- [`crow`](https://github.com/CrowCpp/Crow) - Our REST-Web framework
- [`yaml-cpp`](https://github.com/jbeder/yaml-cpp) - YAML parser
- [`jwt-cpp`](https://github.com/Thalhammer/jwt-cpp) - JSON Web Token library
- [`openssl`](https://github.com/openssl/openssl) - Crypto library
- [`catch2`](https://github.com/catchorg/Catch2) - Testing framework

## 📚 Documentation

For more detailed information, check out our [full documentation](link-to-your-docs).

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for more details.

## 📄 License

flAPI is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for more details.

## 🙋‍♀️ Support

If you have any questions or need help, please [open an issue](https://github.com/yourusername/flapi/issues) or join our [community chat](link-to-your-chat).

---
