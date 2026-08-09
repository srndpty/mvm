function Get-NestedPropertyValue {
    param(
        [Parameter(Mandatory)]
        [object]$InputObject,

        [Parameter(Mandatory)]
        [string]$Path
    )

    $value = $InputObject
    foreach ($segment in $Path.Split('.')) {
        $property = $value.PSObject.Properties[$segment]
        if ($null -eq $property) {
            throw "計測 JSON にプロパティ '$Path' がありません ('$segment' で停止)"
        }
        $value = $property.Value
    }
    return $value
}
