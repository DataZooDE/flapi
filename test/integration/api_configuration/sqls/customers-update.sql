-- Update customer (UPDATE)
-- Note: This uses an in-memory table for testing purposes

UPDATE test_customers
SET
    {{#params.name}}
    name = '{{{ params.name }}}',
    {{/params.name}}
    {{#params.segment}}
    segment = '{{{ params.segment }}}',
    {{/params.segment}}
    {{#params.balance}}
    balance = {{{ params.balance }}},
    {{/params.balance}}
    {{#params.comment}}
    comment = '{{{ params.comment }}}',
    {{/params.comment}}
    {{#params.email}}
    email = '{{{ params.email }}}',
    {{/params.email}}
    {{#params.registration_date}}
    registration_date = DATE '{{{ params.registration_date }}}',
    {{/params.registration_date}}
    {{#params.last_login_time}}
    last_login_time = TIME '{{{ params.last_login_time }}}',
    {{/params.last_login_time}}
    {{#params.uuid}}
    uuid = '{{{ params.uuid }}}'
    {{/params.uuid}}
    {{^params.uuid}}
    -- Ensure at least one field is always updated
    name = name
    {{/params.uuid}}
WHERE id = {{{ params.customer_id }}};

SELECT * FROM test_customers WHERE id = {{{ params.customer_id }}};

