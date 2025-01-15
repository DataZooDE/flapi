<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { cn } from "$lib/utils";
  import { Plus, Trash } from "lucide-svelte";

  interface QueryConfig {
    sql: string;
    parameters: Array<{
      name: string;
      type: "string" | "number" | "boolean";
      required: boolean;
    }>;
  }

  const { query, onUpdate } = $props<{
    query: QueryConfig;
    onUpdate: (q: QueryConfig) => void;
  }>();

  let errors = $state<string[]>([]);

  function validateQuery(): string[] {
    const validationErrors: string[] = [];
    
    if (!query.sql.trim()) {
      validationErrors.push('SQL query is required');
    }

    // Check for duplicate parameter names
    const paramNames = new Set<string>();
    for (const param of query.parameters) {
      if (paramNames.has(param.name)) {
        validationErrors.push(`Duplicate parameter name: ${param.name}`);
      }
      paramNames.add(param.name);
    }

    return validationErrors;
  }

  function handleChange() {
    errors = validateQuery();
    if (errors.length === 0) {
      onUpdate(query);
    }
  }

  function addParameter() {
    query.parameters.push({
      name: '',
      type: 'string',
      required: false
    });
    handleChange();
  }

  function removeParameter(index: number) {
    query.parameters.splice(index, 1);
    handleChange();
  }
</script>

<div class="space-y-4">
  <div class="space-y-2">
    <Label for="sql">SQL Query</Label>
    <textarea
      id="sql"
      bind:value={query.sql}
      onchange={handleChange}
      class="w-full h-32 px-3 py-2 border rounded-md font-mono"
      placeholder="SELECT * FROM table WHERE column = :parameter"
    ></textarea>
  </div>

  <div class="space-y-4">
    <div class="flex justify-between items-center">
      <h3 class="text-lg font-medium">Query Parameters</h3>
      <Button variant="outline" size="sm" onclick={addParameter}>
        <Plus class="h-4 w-4 mr-2" />
        Add Parameter
      </Button>
    </div>

    {#if query.parameters.length > 0}
      {#each query.parameters as param, i}
        <div class="space-y-4 p-4 border rounded-lg">
          <div class="flex justify-between">
            <div class="space-y-2 flex-1 mr-4">
              <Label for={`param-name-${i}`}>Name</Label>
              <Input
                id={`param-name-${i}`}
                bind:value={param.name}
                onchange={handleChange}
                placeholder="Parameter name"
              />
            </div>

            <Button 
              variant="ghost" 
              size="icon"
              onclick={() => removeParameter(i)}
            >
              <Trash class="h-4 w-4" />
            </Button>
          </div>

          <div class="grid grid-cols-2 gap-4">
            <div class="space-y-2">
              <Label for={`param-type-${i}`}>Type</Label>
              <select
                id={`param-type-${i}`}
                bind:value={param.type}
                onchange={handleChange}
                class="w-full px-3 py-2 border rounded-md"
              >
                <option value="string">String</option>
                <option value="number">Number</option>
                <option value="boolean">Boolean</option>
              </select>
            </div>

            <div class="space-y-2">
              <Label for={`param-required-${i}`}>Required</Label>
              <select
                id={`param-required-${i}`}
                bind:value={param.required}
                onchange={handleChange}
                class="w-full px-3 py-2 border rounded-md"
              >
                <option value={true}>Yes</option>
                <option value={false}>No</option>
              </select>
            </div>
          </div>
        </div>
      {/each}
    {/if}
  </div>

  {#if errors.length > 0}
    <div class="text-destructive text-sm space-y-1">
      {#each errors as error}
        <p>{error}</p>
      {/each}
    </div>
  {/if}
</div> 