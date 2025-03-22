SELECT 
  boolean_col,
  int32_col,
  int64_col,
  float_col,
  double_col,
  string_col,
  date_col,
  timestamp_col,
  time_col,
  decimal_col,
  binary_col as binary_col_base64,
  list_col,
  json_col
FROM '{{{conn.path}}}'
LIMIT 100 