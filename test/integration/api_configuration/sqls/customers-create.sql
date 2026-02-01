-- Create customer (INSERT)
-- Note: This uses an in-memory table for testing purposes
-- In a real scenario, you would write to a writable database

CREATE TABLE IF NOT EXISTS test_customers (
    id INTEGER PRIMARY KEY,
    name VARCHAR(100),
    segment VARCHAR(50),
    balance DOUBLE,
    comment TEXT,
    email VARCHAR(100),
    registration_date DATE,
    last_login_time TIME,
    uuid VARCHAR(36)
);

INSERT INTO test_customers (
    id,
    name,
    segment,
    balance,
    comment,
    email,
    registration_date,
    last_login_time,
    uuid
)
SELECT
    COALESCE(MAX(id), 0) + 1,
    '{{{ params.name }}}',
    '{{{ params.segment }}}',
    {{#params.balance}}
    {{{ params.balance }}}
    {{/params.balance}}
    {{^params.balance}}
    0.0
    {{/params.balance}},
    {{#params.comment}}
    '{{{ params.comment }}}'
    {{/params.comment}}
    {{^params.comment}}
    NULL
    {{/params.comment}},
    {{#params.email}}
    '{{{ params.email }}}'
    {{/params.email}}
    {{^params.email}}
    NULL
    {{/params.email}},
    {{#params.registration_date}}
    DATE '{{{ params.registration_date }}}'
    {{/params.registration_date}}
    {{^params.registration_date}}
    NULL
    {{/params.registration_date}},
    {{#params.last_login_time}}
    TIME '{{{ params.last_login_time }}}'
    {{/params.last_login_time}}
    {{^params.last_login_time}}
    NULL
    {{/params.last_login_time}},
    {{#params.uuid}}
    '{{{ params.uuid }}}'
    {{/params.uuid}}
    {{^params.uuid}}
    NULL
    {{/params.uuid}}
FROM test_customers;

-- Select the inserted record using max(id)
SELECT * FROM test_customers WHERE id = (SELECT MAX(id) FROM test_customers);
