<script lang="ts">
  import type { EndpointConfig } from '$lib/types';

  export let endpoint: EndpointConfig;
  let expandedFields: { [key: number]: boolean } = {};

  function addRequestField() {
    endpoint.requestFields = [
      ...endpoint.requestFields,
      {
        fieldName: '',
        fieldIn: 'query',
        description: '',
        required: false,
        validators: []
      }
    ];
  }

  function removeRequestField(index: number) {
    endpoint.requestFields = endpoint.requestFields.filter((_, i) => i !== index);
  }

  function addValidator(fieldIndex: number) {
    endpoint.requestFields[fieldIndex].validators = [
      ...endpoint.requestFields[fieldIndex].validators,
      { type: 'string', preventSqlInjection: true }
    ];
  }

  function removeValidator(fieldIndex: number, validatorIndex: number) {
    endpoint.requestFields[fieldIndex].validators = endpoint.requestFields[
      fieldIndex
    ].validators.filter((_, i) => i !== validatorIndex);
  }

  function toggleField(index: number) {
    expandedFields[index] = !expandedFields[index];
    expandedFields = expandedFields;
  }
</script>

<div class="space-y-4">
  <div class="flex justify-between items-center">
    <h2 class="text-lg font-semibold">Request Fields</h2>
    <button type="button" class="btn btn-secondary" on:click={addRequestField}>
      <span class="mr-2">+</span>
      Add Field
    </button>
  </div>

  <div class="space-y-2">
    {#each endpoint.requestFields as field, fieldIndex}
      <div class="card bg-base-200">
        <!-- Field Header -->
        <div class="p-3 flex items-center justify-between">
          <div class="flex items-center space-x-4 flex-1">
            <button
              type="button"
              class="btn btn-ghost btn-sm p-0 w-6"
              on:click={() => toggleField(fieldIndex)}
            >
              {#if expandedFields[fieldIndex]}
                <span>▼</span>
              {:else}
                <span>▶</span>
              {/if}
            </button>
            <div class="flex items-center space-x-2 flex-1">
              <input
                type="text"
                bind:value={field.fieldName}
                placeholder="Field name"
                class="input input-sm flex-1"
                required
              />
              <select bind:value={field.fieldIn} class="select select-sm">
                <option value="query">Query</option>
                <option value="path">Path</option>
                <option value="header">Header</option>
                <option value="body">Body</option>
              </select>
              <label class="flex items-center space-x-2">
                <input
                  type="checkbox"
                  bind:checked={field.required}
                  class="checkbox checkbox-sm"
                />
                <span class="text-sm">Required</span>
              </label>
            </div>
          </div>
          <button
            type="button"
            class="btn btn-error btn-sm"
            on:click={() => removeRequestField(fieldIndex)}
          >
            <span>×</span>
          </button>
        </div>

        <!-- Expanded Content -->
        {#if expandedFields[fieldIndex]}
          <div class="px-4 pb-4 pt-2 border-t border-base-300">
            <div class="space-y-4">
              <div class="space-y-2">
                <label for="fieldDescription" class="label">Description</label>
                <input
                  id="fieldDescription"
                  type="text"
                  bind:value={field.description}
                  class="input input-sm w-full"
                  placeholder="Field description"
                />
              </div>

              <!-- Validators -->
              <div class="space-y-2">
                <div class="flex justify-between items-center">
                  <h3 class="font-medium">Validators</h3>
                  <button
                    type="button"
                    class="btn btn-secondary btn-sm"
                    on:click={() => addValidator(fieldIndex)}
                  >
                    <span class="mr-2">+</span>
                    Add Validator
                  </button>
                </div>

                <div class="space-y-2">
                  {#each field.validators as validator, validatorIndex}
                    <div class="card bg-base-100 p-3">
                      <div class="flex items-center space-x-4">
                        <select bind:value={validator.type} class="select select-sm">
                          <option value="string">String</option>
                          <option value="int">Integer</option>
                          <option value="enum">Enum</option>
                          <option value="date">Date</option>
                          <option value="time">Time</option>
                        </select>

                        {#if validator.type === 'string'}
                          <input
                            type="text"
                            bind:value={validator.regex}
                            class="input input-sm flex-1"
                            placeholder="Regex pattern"
                          />
                        {:else if validator.type === 'int'}
                          <input
                            type="number"
                            bind:value={validator.min}
                            class="input input-sm w-24"
                            placeholder="Min"
                          />
                          <input
                            type="number"
                            bind:value={validator.max}
                            class="input input-sm w-24"
                            placeholder="Max"
                          />
                        {/if}

                        <label class="flex items-center space-x-2">
                          <input
                            type="checkbox"
                            bind:checked={validator.preventSqlInjection}
                            class="checkbox checkbox-sm"
                          />
                          <span class="text-sm">Prevent SQL Injection</span>
                        </label>

                        <button
                          type="button"
                          class="btn btn-error btn-sm"
                          on:click={() => removeValidator(fieldIndex, validatorIndex)}
                        >
                          <span>×</span>
                        </button>
                      </div>
                    </div>
                  {/each}
                </div>
              </div>
            </div>
          </div>
        {/if}
      </div>
    {/each}
  </div>
</div>
