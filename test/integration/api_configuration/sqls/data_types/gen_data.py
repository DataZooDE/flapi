#!/usr/bin/env python3

import random
import string


import pandas as pd
import numpy as np
from datetime import datetime, time

# Create a DataFrame with various data types
df = pd.DataFrame({
    'boolean_col': np.random.choice([True, False], 1000),
    'int32_col': np.random.randint(-2**31, 2**31-1, 1000, dtype=np.int32),
    'int64_col': np.random.randint(-2**63, 2**63-1, 1000, dtype=np.int64),
    'float_col': np.random.random(1000).astype(np.float32),
    'double_col': np.random.random(1000),
    'string_col': np.random.choice(['A', 'B', 'C', 'D', 'E'], 1000),
    'date_col': pd.date_range(start='2023-01-01', periods=1000),
    'timestamp_col': pd.date_range(start='2023-01-01', periods=1000, freq='s'),
    'time_col': [time(hour=np.random.randint(0, 24), minute=np.random.randint(0, 60), second=np.random.randint(0, 60)) for _ in range(1000)],
    'decimal_col': pd.Series([np.random.randint(0, 1000000) / 100 for _ in range(1000)]).astype('float64'),
    'binary_col': [bytes([np.random.randint(0, 256) for _ in range(10)]) for _ in range(1000)],
    'list_col': [[np.random.randint(1, 100) for _ in range(np.random.randint(1, 5))] for _ in range(1000)],
    'json_col': [{'id': i, 'value': np.random.random()} for i in range(1000)]
})

# Save as parquet file
df.to_parquet('./api_configuration/data/data_types.parquet', engine='pyarrow')