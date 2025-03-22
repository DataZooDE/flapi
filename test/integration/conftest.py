import pytest
import subprocess
import time
import signal
import os
import pathlib
import base64
from datetime import datetime
import uuid
import json

@pytest.fixture(scope="session")
def flapi_server():
    # Get the current directory where conftest.py is located
    current_dir = pathlib.Path(__file__).parent
    config_path = current_dir / "api_configuration" / "flapi.yaml"
    
    # Assuming flapi binary is two directories up in build/release
    flapi_binary = current_dir.parent.parent / "build" / "release" / "flapi"
    
    # Start flapi binary with configuration
    process = subprocess.Popen(
        [
            str(flapi_binary),
            "-c", str(config_path),
            "--log-level", "debug"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        preexec_fn=os.setsid
    )
    
    # Wait for server to start
    time.sleep(2)  # Adjust as needed
    
    yield process
    
    # Cleanup: Kill the server
    os.killpg(os.getpgid(process.pid), signal.SIGTERM)
    process.wait()

@pytest.fixture
def auth_headers():
    # You'll need to provide the actual credentials
    username = "test_user"
    password = "test_pass"
    credentials = base64.b64encode(f"{username}:{password}".encode()).decode()
    return {"Authorization": f"Basic {credentials}"}

@pytest.fixture(scope="session", autouse=True)
def wait_for_api(flapi_server):
    """Ensure API has started before running tests"""
    max_retries = 30
    retry_interval = 1
    
    import requests
    from requests.exceptions import ConnectionError
    
    for _ in range(max_retries):
        try:
            # Health endpoint might not require auth
            response = requests.get("http://localhost:8080")
            if response.status_code == 200:
                return
        except ConnectionError:
            time.sleep(retry_interval)
            
    raise Exception("API failed to start")

def verify_data_types(response, expected_types):
    """Verify that the data types in the response match the expected types."""
    if not response.get("data") or not isinstance(response["data"], list):
        raise ValueError("Response data must be a non-empty list")
    
    # Check first row for type verification
    first_row = response["data"][0]
    
    for column, expected_type in expected_types.items():
        if column not in first_row:
            raise ValueError(f"Column {column} not found in response")
        
        value = first_row[column]
        
        # Skip null values
        if value is None:
            continue
            
        # Verify types
        if expected_type == "!integer":
            if not isinstance(value, int):
                raise ValueError(f"Column {column} expected integer, got {type(value)}")
        
        elif expected_type == "!float":
            if not isinstance(value, (float, int)):
                raise ValueError(f"Column {column} expected float, got {type(value)}")
        
        elif expected_type == "!string":
            if not isinstance(value, str):
                raise ValueError(f"Column {column} expected string, got {type(value)}")
        
        elif expected_type == "!boolean":
            if not isinstance(value, bool):
                raise ValueError(f"Column {column} expected boolean, got {type(value)}")
        
        elif expected_type == "!anylist":
            if not isinstance(value, list):
                raise ValueError(f"Column {column} expected array, got {type(value)}")
        
        elif expected_type == "!anyjson":
            # Try to parse if it's a string
            if isinstance(value, str):
                try:
                    json.loads(value)
                except json.JSONDecodeError:
                    raise ValueError(f"Column {column} expected valid JSON")
            elif not isinstance(value, (dict, list)):
                raise ValueError(f"Column {column} expected JSON object or array, got {type(value)}")

    return True

@pytest.fixture
def setup_data_types_table():
    """Fixture to ensure the data_types table exists and has test data."""
    # This would be implemented based on your setup needs
    pass 