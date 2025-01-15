<script lang="ts">
  import { Input } from "$lib/components/ui/input";
  import { Label } from "$lib/components/ui/label";
  import { Button } from "$lib/components/ui/button";
  import { cn } from "$lib/utils";
  import { Plus, Trash } from "lucide-svelte";

  const { parameters, onUpdate } = $props<{
    parameters: Array<{
      name: string;
      type: "string" | "number" | "boolean";
      in: "path" | "query" | "header";
      required: boolean;
      description?: string;
    }>;
    onUpdate: (params: typeof parameters) => void;
  }>();

  let errors = $state<string[][]>([]);

  function validateParameter(param: typeof parameters[0]): string[] {
    const validationErrors: string[] = [];
    
    if (!param.name) {
      validationErrors.push('Parameter name is required');
    } else if (!/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(param.name)) {
      validationErrors.push('Parameter name must start with a letter or underscore');
    }

    return validationErrors;
  }

  function handleChange() {
    errors = parameters.map(validateParameter);
    if (errors.every(e => e.length === 0)) {
      onUpdate(parameters);
    }
  }

  function addParameter() {
    parameters.push({
      name: '',
      type: 'string',
      in: 'query',
      required: false
    });
    handleChange();
  }

  function removeParameter(index: number) {
    parameters.splice(index, 1);
    handleChange();
  }
</script>

<div class="space-y-4">
  <div class="flex justify-between items-center">
    <h3 class="text-lg font-medium">Parameters</h3>
    <Button variant="outline" size="sm" onclick={addParameter}>
      <Plus class="h-4 w-4 mr-2" />
      Add Parameter
    </Button>
  </div>

  {#if parameters.length > 0}
    {#each parameters as param, i}
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
            <Label for={`param-in-${i}`}>Location</Label>
            <select
              id={`param-in-${i}`}
              bind:value={param.in}
              onchange={handleChange}
              class="w-full px-3 py-2 border rounded-md"
            >
              <option value="query">Query</option>
              <option value="path">Path</option>
              <option value="header">Header</option>
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

        {#if errors[i]?.length > 0}
          <div class="text-destructive text-sm space-y-1">
            {#each errors[i] as error}
              <p>{error}</p>
            {/each}
          </div>
        {/if}
      </div>
    {/each}
  {/if}
</div> 